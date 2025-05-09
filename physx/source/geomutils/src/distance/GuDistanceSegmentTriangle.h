// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//  * Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  * Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//  * Neither the name of NVIDIA CORPORATION nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ''AS IS'' AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
// OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// Copyright (c) 2008-2025 NVIDIA Corporation. All rights reserved.
// Copyright (c) 2004-2008 AGEIA Technologies, Inc. All rights reserved.
// Copyright (c) 2001-2004 NovodeX AG. All rights reserved.  

#ifndef GU_DISTANCE_SEGMENT_TRIANGLE_H
#define GU_DISTANCE_SEGMENT_TRIANGLE_H

#include "common/PxPhysXCommonConfig.h"
#include "GuSegment.h"
#include "foundation/PxVecMath.h"

namespace physx
{
namespace Gu
{
	PX_PHYSX_COMMON_API PxReal distanceSegmentTriangleSquared(
		const PxVec3& segmentOrigin, const PxVec3& segmentExtent,
		const PxVec3& triangleOrigin, const PxVec3& triangleEdge0, const PxVec3& triangleEdge1,
		PxReal* t=NULL, PxReal* u=NULL, PxReal* v=NULL);

	PX_INLINE PxReal distanceSegmentTriangleSquared(
		const Gu::Segment& segment, 
		const PxVec3& triangleOrigin, const PxVec3& triangleEdge0, const PxVec3& triangleEdge1,
		PxReal* t=NULL, PxReal* u=NULL, PxReal* v=NULL)
	{
		return distanceSegmentTriangleSquared(
			segment.p0, segment.computeDirection(), triangleOrigin, triangleEdge0, triangleEdge1, t, u, v);
	}

	//	closest0 is the closest point on segment pq
	//	closest1 is the closest point on triangle abc
	PX_PHYSX_COMMON_API aos::FloatV distanceSegmentTriangleSquared(
		const aos::Vec3VArg p, const aos::Vec3VArg q,
		const aos::Vec3VArg a, const aos::Vec3VArg b, const aos::Vec3VArg c,
		aos::Vec3V& closest0, aos::Vec3V& closest1);

} // namespace Gu
}

#endif
