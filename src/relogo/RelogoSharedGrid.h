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
 *     Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *
 *     Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the documentation
 *     and/or other materials provided with the distribution.
 *
 *     Neither the name of the Argonne National Laboratory nor the names of its
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
 *  RelogoSharedGrid.h
 *
 *  Created on: Sep 7, 2010
 *      Author: nick
 */

#ifndef RELOGOSHAREDGRID_H_
#define RELOGOSHAREDGRID_H_

#include <boost/mpi/communicator.hpp>

#include "repast_hpc/SharedSpace.h"

namespace repast {
namespace relogo {

template <typename GPTransformer, typename Adder>
class RelogoSharedGrid : public repast::SharedGrid<RelogoAgent, GPTransformer, Adder> {

public:
	virtual ~RelogoSharedGrid() {}
	RelogoSharedGrid(std::string name, repast::GridDimensions gridDims, std::vector<int> processDims, int buffer, boost::mpi::communicator* world);
};

template <typename GPTransformer, typename Adder>
RelogoSharedGrid<GPTransformer, Adder>::RelogoSharedGrid(std::string name, repast::GridDimensions gridDims, std::vector<int> processDims, int buffer, boost::mpi::communicator* world) :
repast::SharedGrid<RelogoAgent, GPTransformer, Adder>(name, gridDims, processDims, buffer, world) {}




}
}


#endif /* RELOGOSHAREDGRID_H_ */
