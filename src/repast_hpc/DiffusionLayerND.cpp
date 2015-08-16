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
 *  DiffusionLayerND.cpp
 *
 *  Created on: July 25, 2008
 *      Author: jtm
 */

#include "DiffusionLayerND.h"
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

  // Set these provisionally; adjust below if needed
  simplifiedBoundariesMin  = localBoundariesMin;
  if(spaceContinuesLeft)  simplifiedBoundariesMin -= leftBufferSize;

  simplifiedBoundariesMax  = localBoundariesMax;
  if(spaceContinuesRight) simplifiedBoundariesMax += rightBufferSize;

  matchingCoordinateMin    = localBoundariesMin;
  if(spaceContinuesLeft  && !atLeftBound ) matchingCoordinateMin -= leftBufferSize;

  matchingCoordinateMax    = localBoundariesMax;
  if(spaceContinuesRight && !atRightBound) matchingCoordinateMax += rightBufferSize;

  globalWidth = globalCoordinateMax - globalCoordinateMin;
  localWidth = localBoundariesMax - localBoundariesMin;
  width = simplifiedBoundariesMax - simplifiedBoundariesMin;
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


DiffusionLayerND::DiffusionLayerND(vector<int> processesPerDim, GridDimensions globalBoundaries, int bufferSize, bool periodic, double initialValue): globalSpaceIsPeriodic(periodic){
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

  neighborData = new RankDatum[relLoc.getMaxIndex() - 1];
  neighborCount = 0;
  int i = 0;
  do{
    if(relLoc.validNonCenter()){ // Skip 0,0,0,0,0
      RankDatum* datum;
      datum = &neighborData[i];
      // Collect the information about this rank here
      getMPIDataType(relLoc, numDims - 1, datum->datatype);
      datum->sendPtrOffset    = getSendPointerOffset(relLoc);
      datum->receivePtrOffset = getReceivePointerOffset(relLoc);
      neighborCount++;
    }
  }while(relLoc.increment());

  // Create the actual arrays for the data
  dataSpace1 = new double[length];
  dataSpace2 = new double[length];
  currentDataSpace = dataSpace1;
  otherDataSpace   = dataSpace2;

  // Create arrays for MPI requests and results (statuses)
  requests = new MPI_Request[neighborCount];

  // Finally, fill the data with the initial values
  initialize(initialValue);

}

DiffusionLayerND::~DiffusionLayerND(){
  delete[] currentDataSpace;
  delete[] otherDataSpace;
  delete[] neighborData; // Should Free MPI Datatypes first...
  delete[] requests;
}


void DiffusionLayerND::initialize(double initialValue){
  for(int i = 0; i < length; i++){ // TODO Optimize
    dataSpace1[i] = initialValue;
    dataSpace2[i] = initialValue;
  }
}

void DiffusionLayerND::diffuse(){

  // Switch the data banks
  double* tempDataSpace = currentDataSpace;
  currentDataSpace      = otherDataSpace;
  otherDataSpace        = tempDataSpace;

  synchronize();
}


vector<int> DiffusionLayerND::transform(vector<int> location){
  vector<int> ret;
  ret.assign(numDims, 0); // Make the right amount of space
  for(int i = 0; i < numDims; i++) ret[i] = dimensionData[i].getTransformedCoord(location[i]);
  return ret;
}

int DiffusionLayerND::getIndex(vector<int> location){
  vector<int> transformed = transform(location);
  int val = 0;
  for(int i = numDims - 1; i >= 0; i--){
    val += transformed[i] * places[i];
  }
}

int DiffusionLayerND::getIndex(Point<int> location){
  return getIndex(location.coords());
}

void DiffusionLayerND::getMPIDataType(RelativeLocation relLoc, int dimensionIndex, MPI_Datatype& datatype){

  if(dimensionIndex == 0){
    MPI_Type_contiguous(dimensionData[dimensionIndex].getSendReceiveSize(relLoc[dimensionIndex]), MPI_DOUBLE, &datatype);
  }
  else{
    MPI_Datatype innerType;
    getMPIDataType(relLoc, dimensionIndex - 1, innerType);
    MPI_Type_hvector(dimensionData[dimensionIndex].getSendReceiveSize(relLoc[dimensionIndex]), // Count
                     1,                                                                        // BlockLength: just one of the inner data type
                     strides[dimensionIndex],                                                  // Stride, in bytes
                     innerType,                                                                // Inner Datatype
                     &datatype);
  }
  // Commit?
  MPI_Type_commit(&datatype);
}

void DiffusionLayerND::synchronize(){
  // For each entry in neighbors:
  MPI_Status* statuses = new MPI_Status[neighborCount];
  for(int i = 0; i < neighborCount; i++){
    MPI_Isend(&currentDataSpace[neighborData[i].sendPtrOffset], 1, neighborData[i].datatype,
        neighborData[i].rank, 10101, cartTopology->topologyComm, &requests[i]);
    MPI_Irecv(&currentDataSpace[neighborData[i].receivePtrOffset], 1, neighborData[i].datatype,
        neighborData[i].rank, 10101, cartTopology->topologyComm, &requests[i + 1]);
  }
  // Wait
  MPI_Waitall(neighborCount, requests, statuses);
  delete[] statuses;

  // Done! Data will be in the new buffer in the current data space
}

int DiffusionLayerND::getSendPointerOffset(RelativeLocation relLoc){
  int rank = repast::RepastProcess::instance()->rank();
  int ret = 0;
  for(int i = 0; i < numDims; i++){
    DimensionDatum* datum = &dimensionData[i];
    ret += (relLoc[i] <= 0 ? datum->leftBufferSize : datum->width - datum->rightBufferSize) * places[i];
  }
  return ret;
}

int DiffusionLayerND::getReceivePointerOffset(RelativeLocation relLoc){
  int rank = repast::RepastProcess::instance()->rank();
  int ret = 0;
  for(int i = 0; i < numDims; i++){
    DimensionDatum* datum = &dimensionData[i];
    ret += (relLoc[i] < 0 ? 0 : (relLoc[i] == 0 ? datum->leftBufferSize : datum->width - datum->rightBufferSize)) * places[i];
  }
  return ret;
}


double DiffusionLayerND::addValueAt(double val, Point<int> location){
  double* pt = &currentDataSpace[getIndex(location)];
  return (*pt = *pt + val);
}

double DiffusionLayerND::addValueAt(double val, vector<int> location){
  double* pt = &currentDataSpace[getIndex(location)];
  return (*pt = *pt + val);
}

double DiffusionLayerND::setValueAt(double val, Point<int> location){
  double* pt = &currentDataSpace[getIndex(location)];
  return (*pt = val);

}

double DiffusionLayerND::setValueAt(double val, vector<int> location){
  double* pt = &currentDataSpace[getIndex(location)];
  return (*pt = val);
}


}
