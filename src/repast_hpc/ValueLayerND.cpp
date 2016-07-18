/*
 *   Repast for High Performance Computing (Repast HPC)
 *
 *   Copyright (c) 2010 Argonne National Laboratory
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with
 *   or without modification, are permitted provided that the following
 *   conditions are met:
 *
 *  	 Redistributions of source code must retain the above copyright notice,
 *  	 this list of conditions and the following disclaimer.
 *
 *  	 Redistributions in binary form must reproduce the above copyright notice,
 *  	 this list of conditions and the following disclaimer in the documentation
 *  	 and/or other materials provided with the distribution.
 *
 *  	 Neither the name of the Argonne National Laboratory nor the names of its
 *     contributors may be used to endorse or promote products derived from
 *     this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 *   PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE TRUSTEES OR
 *   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 *   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 *   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 *   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 *   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 *   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 *   EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *
 *  ValueLayerND.cpp
 *
 *  Created on: July 18, 2016
 *      Author: jtm
 */
#include "ValueLayerND.h"
#include "RepastProcess.h"
#include "Point.h"

#include <boost/mpi.hpp>

using namespace std;

namespace repast {


DimensionDatum::DimensionDatum(int indx, GridDimensions globalBoundaries, GridDimensions localBoundaries, int buffer, bool isPeriodic):
    leftBufferSize(buffer), rightBufferSize(buffer), periodic(isPeriodic){
  globalCoordinateMin = globalBoundaries.origin(indx);
  globalCoordinateMax = globalBoundaries.origin(indx) + globalBoundaries.extents(indx);
  localBoundariesMin  = localBoundaries.origin(indx);
  localBoundariesMax  = localBoundaries.origin(indx) + localBoundaries.extents(indx);

  atLeftBound  = localBoundariesMin == globalCoordinateMin;
  atRightBound = localBoundariesMax == globalCoordinateMax;

  spaceContinuesLeft  = !atLeftBound  || periodic;
  spaceContinuesRight = !atRightBound || periodic;

  simplifiedBoundariesMin  = localBoundariesMin - leftBufferSize;
  simplifiedBoundariesMax  = localBoundariesMax + rightBufferSize;

  matchingCoordinateMin    = localBoundariesMin;
  if(spaceContinuesLeft  && !atLeftBound ) matchingCoordinateMin -= leftBufferSize;

  matchingCoordinateMax    = localBoundariesMax;
  if(spaceContinuesRight && !atRightBound) matchingCoordinateMax += rightBufferSize;

  globalWidth = globalCoordinateMax - globalCoordinateMin;
  localWidth = localBoundariesMax - localBoundariesMin;
  width = leftBufferSize + localWidth + rightBufferSize;
  widthInBytes = width * (sizeof(double));
}

int DimensionDatum::getSendReceiveSize(int relativeLocation){
  switch(relativeLocation){
    case -1:  return leftBufferSize;
    case  1:  return rightBufferSize;
    case  0:
    default:
      return localWidth;
  }
}

int DimensionDatum::getTransformedCoord(int originalCoord){
  if(originalCoord < matchingCoordinateMin){        // Assume (!) original is on right (!) side of periodic boundary, starting at some value
    return matchingCoordinateMax + (originalCoord - globalCoordinateMin);
  }
  else if(originalCoord > matchingCoordinateMax){
    return matchingCoordinateMin - (globalCoordinateMax - originalCoord);
  }
  else return originalCoord; // Within matching boundaries; no need to transform

}


int DimensionDatum::getIndexedCoord(int originalCoord, bool isSimplified){
  return (isSimplified ? originalCoord : getTransformedCoord(originalCoord)) - simplifiedBoundariesMin;
}

bool DimensionDatum::isInLocalBounds(int originalCoord){
  return originalCoord >= localBoundariesMin && originalCoord < localBoundariesMax;
}





int ValueLayerND::syncCount = 0;

ValueLayerND::ValueLayerND(vector<int> processesPerDim, GridDimensions globalBoundaries, int bufferSize, bool periodic,
    double initialValue, double initialBufferZoneValue): globalSpaceIsPeriodic(periodic){
  cartTopology = RepastProcess::instance()->getCartesianTopology(processesPerDim, periodic);
  int rank = RepastProcess::instance()->rank();
  GridDimensions localBoundaries = cartTopology->getDimensions(rank, globalBoundaries);

  // Calculate the size to be used for the buffers
  numDims = processesPerDim.size();

  // First create the basic coordinate data per dimension
  length = 1;
  int val = 1;
  for(int i = 0; i < numDims; i++){
    DimensionDatum datum(i, globalBoundaries, localBoundaries, bufferSize, periodic);
    length *= datum.width;
    dimensionData.push_back(datum);
    places.push_back(val);
    strides.push_back(val * sizeof(double));
    val *= dimensionData[i].width;
  }

  // Now create the rank-based data per neighbor
  RelativeLocation relLoc(numDims);
  RelativeLocation relLocTrimmed = cartTopology->trim(rank, relLoc); // Initialized to minima

  vector<int> myCoordinates;
  cartTopology->getCoordinates(rank, myCoordinates);

  neighborData = new RankDatum[relLoc.getMaxIndex()];
  neighborCount = 0;
  int i = 0;
  do{
    if(relLoc.validNonCenter()){ // Skip 0,0,0,0,0
      RankDatum* datum;
      datum = &neighborData[neighborCount];
      // Collect the information about this rank here
      getMPIDataType(relLoc, datum->datatype);
      datum->sendPtrOffset    = getSendPointerOffset(relLoc);
      datum->receivePtrOffset = getReceivePointerOffset(relLoc);
      vector<int> current = relLoc.getCurrentValue();
      datum->rank = cartTopology->getRank(myCoordinates, current);
      datum->sendDir = RelativeLocation::getDirectionIndex(current);
      datum->recvDir = RelativeLocation::getReverseDirectionIndex(current);

      neighborCount++;
    }
  }while(relLoc.increment());

  // Create the actual arrays for the data
  dataSpace = new double[length];

  // Create arrays for MPI requests and results (statuses)
  requests = new MPI_Request[neighborCount * 2];

  // Finally, fill the data with the initial values
  initialize(initialValue, initialBufferZoneValue);

  // And synchronize
  synchronize();

}

ValueLayerND::~ValueLayerND(){
  delete[] dataSpace;
  delete[] neighborData; // Should Free MPI Datatypes first...
  delete[] requests;
}


void ValueLayerND::initialize(double initialValue, bool fillBufferZone, bool fillLocal){
  fillDimension(initialValue, initialValue, fillBufferZone, fillLocal, dataSpace, numDims - 1);
}

void ValueLayerND::initialize(double initialLocalValue, double initialBufferZoneValue){
  fillDimension(initialLocalValue, initialBufferZoneValue, true, true, dataSpace, numDims - 1);
}

vector<int> ValueLayerND::getIndexes(vector<int> location, bool isSimplified){
  vector<int> ret;
  ret.assign(numDims, 0); // Make the right amount of space
  for(int i = 0; i < numDims; i++) ret[i] = dimensionData[i].getIndexedCoord(location[i], isSimplified);
  return ret;
}

int ValueLayerND::getIndex(vector<int> location, bool isSimplified){
  vector<int> indexed = getIndexes(location, isSimplified);
  int val = 0;
  for(int i = numDims - 1; i >= 0; i--) val += indexed[i] * places[i];
  if(val < 0 || val > length) val = -1;
  return val;
}

int ValueLayerND::getIndex(Point<int> location){
  return getIndex(location.coords());
}

void ValueLayerND::getMPIDataType(RelativeLocation relLoc, MPI_Datatype &datatype){
  vector<int> sideLengths;
  for(int i = 0; i < numDims; i++) sideLengths.push_back(dimensionData[i].getSendReceiveSize(relLoc[i]));
  getMPIDataType(sideLengths, datatype, numDims - 1);
}

void ValueLayerND::getMPIDataType(int radius, MPI_Datatype &datatype){
  vector<int> sideLengths;
  sideLengths.assign(numDims, 2 * radius + 1);
  getMPIDataType(sideLengths, datatype, numDims - 1);
}

void ValueLayerND::getMPIDataType(vector<int> sideLengths, MPI_Datatype &datatype, int dimensionIndex){
  if(dimensionIndex == 0){
    MPI_Type_contiguous(sideLengths[dimensionIndex], MPI_DOUBLE, &datatype);
  }
  else{
    MPI_Datatype innerType;
    getMPIDataType(sideLengths, innerType, dimensionIndex - 1);
    MPI_Type_hvector(sideLengths[dimensionIndex], // Count
                     1,                                                                        // BlockLength: just one of the inner data type
                     strides[dimensionIndex],                                                  // Stride, in bytes
                     innerType,                                                                // Inner Datatype
                     &datatype);
  }
  // Commit?
  MPI_Type_commit(&datatype);
}


void ValueLayerND::synchronize(){
  syncCount++;
  if(syncCount > 9) syncCount = 0;
  // Note: the syncCount and send/recv directions are used to create a unique tag value for the
  // mpi sends and receives. The tag value must be unique in two ways: first, successive calls to this
  // function must be different enough that they can't be confused. The 'syncCount' value is used to
  // achieve this, and it will loop from 0-9 and then repeat. The second, the tag must sometimes
  // differentiate between sends and receives that are going to the same rank. If a dimension
  // has only 2 processes but wrap-around borders, then one process may be sending to the other
  // process twice (once left and once right). The 'sendDir' and 'recvDir' values trap this

  // For each entry in neighbors:
  MPI_Status statuses[neighborCount * 2];
  for(int i = 0; i < neighborCount; i++){
    MPI_Isend(&dataSpace[neighborData[i].sendPtrOffset], 1, neighborData[i].datatype,
        neighborData[i].rank, 10 * (neighborData[i].sendDir + 1) + syncCount, cartTopology->topologyComm, &requests[i]);
    MPI_Irecv(&dataSpace[neighborData[i].receivePtrOffset], 1, neighborData[i].datatype,
        neighborData[i].rank, 10 * (neighborData[i].recvDir + 1) + syncCount, cartTopology->topologyComm, &requests[neighborCount + i]);
  }
  int ret = MPI_Waitall(neighborCount, requests, statuses);
}

int ValueLayerND::getSendPointerOffset(RelativeLocation relLoc){
  int rank = repast::RepastProcess::instance()->rank();
  int ret = 0;
  for(int i = 0; i < numDims; i++){
    DimensionDatum* datum = &dimensionData[i];
    ret += (relLoc[i] <= 0 ? datum->leftBufferSize : datum->width - (2 * datum->rightBufferSize)) * places[i];
  }
  return ret;
}

int ValueLayerND::getReceivePointerOffset(RelativeLocation relLoc){
  int rank = repast::RepastProcess::instance()->rank();
  int ret = 0;
  for(int i = 0; i < numDims; i++){
    DimensionDatum* datum = &dimensionData[i];
    ret += (relLoc[i] < 0 ? 0 : (relLoc[i] == 0 ? datum->leftBufferSize : datum->width - datum->rightBufferSize)) * places[i];
  }
  return ret;
}

bool ValueLayerND::isInLocalBounds(vector<int> coords){
  for(int i = 0; i < numDims; i++){
    DimensionDatum* datum = &dimensionData[i];
    if(!datum->isInLocalBounds(coords[i])) return false;
  }
  return true;
}

bool ValueLayerND::isInLocalBounds(Point<int> location){
  return isInLocalBounds(location.coords());
}

double ValueLayerND::addValueAt(double val, Point<int> location){
  int indx = getIndex(location);
  if(indx == -1) return nan("");
  double* pt = &dataSpace[indx];
  return (*pt = *pt + val);
}

double ValueLayerND::addValueAt(double val, vector<int> location){
  int indx = getIndex(location);
  if(indx == -1) return nan("");
  double* pt = &dataSpace[indx];
  return (*pt = *pt + val);
}

double ValueLayerND::setValueAt(double val, Point<int> location){
  int indx = getIndex(location);
  if(indx == -1) return nan("");
  double* pt = &dataSpace[indx];
  return (*pt = val);
}

double ValueLayerND::setValueAt(double val, vector<int> location){
  int indx = getIndex(location);
  if(indx == -1) return nan("");
  double* pt = &dataSpace[indx];
  return (*pt = val);
}

double ValueLayerND::getValueAt(vector<int> location){
  int indx = getIndex(location);
  if(indx == -1) return nan("");
  return dataSpace[indx];
}

void ValueLayerND::write(string fileLocation, string fileTag, bool writeSharedBoundaryAreas){
  std::ofstream outfile;
  std::ostringstream stream;
  int rank = repast::RepastProcess::instance()->rank();
  stream << fileLocation << "DiffusionLayer_" << fileTag << "_" << rank << ".csv";
  std::string filename = stream.str();

  const char * c = filename.c_str();
  outfile.open(c, std::ios_base::trunc | std::ios_base::out); // it will not delete the content of file, will add a new line

  // Write headers
  for(int i = 0; i < numDims; i++) outfile << "DIM_" << i << ",";
  outfile << "VALUE" << endl;

  int* positions = new int[numDims];
  for(int i = 0; i < numDims; i++) positions[i] = 0;

  writeDimension(outfile, dataSpace, positions, numDims - 1, writeSharedBoundaryAreas);

  outfile.close();
}

void ValueLayerND::writeDimension(std::ofstream& outfile, double* dataSpacePointer, int* currentPosition, int dimIndex, bool writeSharedBoundaryAreas){
  int bufferEdge = dimensionData[dimIndex].leftBufferSize;
  int localEdge  = bufferEdge + dimensionData[dimIndex].localWidth;
  int upperBound = localEdge + dimensionData[dimIndex].rightBufferSize;

  int pointerIncrement = places[dimIndex];
  int i = 0;
  for(; i < bufferEdge; i++){
    currentPosition[dimIndex] = i;
    if(writeSharedBoundaryAreas){
      if(dimIndex == 0){
        double val = *dataSpacePointer;
        if(val != 0){
          for(int j = 0; j < numDims; j++) outfile << (currentPosition[j] - dimensionData[j].leftBufferSize) << ",";
          outfile << val << endl;
        }
      }
      else{
        writeDimension(outfile, dataSpacePointer, currentPosition, dimIndex - 1, writeSharedBoundaryAreas);
      }
    }
    // Increment the pointers
    dataSpacePointer += pointerIncrement;
  }
  for(; i < localEdge; i++){
    currentPosition[dimIndex] = i;
    if(dimIndex == 0){
        double val = *dataSpacePointer;
        if(val != 0){
          for(int j = 0; j < numDims; j++) outfile << (currentPosition[j] - dimensionData[j].leftBufferSize) << ",";
          outfile << val << endl;
        }
    }
    else{
      writeDimension(outfile, dataSpacePointer, currentPosition, dimIndex - 1, writeSharedBoundaryAreas);
    }
    // Increment the pointers
    dataSpacePointer += pointerIncrement;
  }
  if(writeSharedBoundaryAreas){ // Note: we don't need to finish this at all if not doing buffer zone
    for(; i < upperBound; i++){
      currentPosition[dimIndex] = i;
      if(dimIndex == 0){
        double val = *dataSpacePointer;
        if(val != 0){
          for(int j = 0; j < numDims; j++) outfile << (currentPosition[j] - dimensionData[j].leftBufferSize) << ",";
          outfile << *dataSpacePointer << endl;
        }
      }
      else{
        writeDimension(outfile, dataSpacePointer, currentPosition, dimIndex - 1, writeSharedBoundaryAreas);
      }
    }
    dataSpacePointer += pointerIncrement;
  }

}


void ValueLayerND::fillDimension(double localValue, double bufferValue, bool doBufferZone, bool doLocal, double* dataSpacePointer, int dimIndex){
  if(!doBufferZone && !doLocal) return;
  int bufferEdge = dimensionData[dimIndex].leftBufferSize;
  int localEdge  = bufferEdge + dimensionData[dimIndex].localWidth;
  int upperBound = localEdge + dimensionData[dimIndex].rightBufferSize;

  int pointerIncrement = places[dimIndex];


  int i = 0;
  for(; i < bufferEdge; i++){
    if(doBufferZone){
      if(dimIndex == 0){
        *dataSpacePointer = bufferValue;
      }
      else{
        fillDimension(bufferValue, bufferValue, doBufferZone, doLocal, dataSpacePointer, dimIndex - 1);
      }
    }
    // Increment the pointers
    dataSpacePointer += pointerIncrement;
  }
  for(; i < localEdge; i++){
    if(doLocal){
      if(dimIndex == 0){
        *dataSpacePointer = localValue;
      }
      else{
        fillDimension(localValue, bufferValue, doBufferZone, doLocal, dataSpacePointer, dimIndex - 1);
      }
    }
    // Increment the pointers
    dataSpacePointer += pointerIncrement;
  }
  if(doBufferZone){ // Note: we don't need to finish this at all if not doing buffer zone
    for(; i < upperBound; i++){
      if(dimIndex == 0){
        *dataSpacePointer = bufferValue;
      }
      else{
        fillDimension(bufferValue, bufferValue, doBufferZone, doLocal, dataSpacePointer, dimIndex - 1);
      }
    }
    dataSpacePointer += pointerIncrement;
  }

}


void ValueLayerND::grabDimensionData(double*& destinationPointer, double* startPointer, int radius, int dimIndex){
  int pointerIncrement = places[dimIndex];
  startPointer -= pointerIncrement * radius; // Go back
  int size = 2 * radius + 1;
  for(int i = 0; i < size; i++){
    if(dimIndex == 0){
      *destinationPointer = 1;
      double myVal = *startPointer;
      *destinationPointer = myVal;
      destinationPointer++;                 // Handle; all recursive instances share
    }
    else{
      grabDimensionData(destinationPointer, startPointer, radius, dimIndex - 1);
    }
    startPointer += pointerIncrement;
  }

}



}

