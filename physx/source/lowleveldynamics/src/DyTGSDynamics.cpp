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

#include "foundation/PxTime.h"
#include "foundation/PxAtomic.h"
#include "foundation/PxSIMDHelpers.h"
#include "PxvDynamics.h"

#include "common/PxProfileZone.h"
#include "PxsRigidBody.h"
#include "PxsContactManager.h"
#include "DyTGSDynamics.h"
#include "DyBodyCoreIntegrator.h"
#include "DySolverCore.h"
#include "DySolverControl.h"
#include "DySolverContact.h"
#include "DyArticulationContactPrep.h"
#include "DySolverBody.h"

#include "DyConstraintPrep.h"
#include "DyConstraintPartition.h"

#include "CmFlushPool.h"
#include "DyArticulationPImpl.h"
#include "PxsMaterialManager.h"
#include "DyContactReduction.h"
#include "PxcNpContactPrepShared.h"
#include "DyContactPrep.h"
#include "PxSceneDesc.h"
#include "PxsSimpleIslandManager.h"
#include "PxvNphaseImplementationContext.h"
#include "PxsContactManagerState.h"
#include "DyContactPrepShared.h"
#include "DySolverContext.h"
#include "DyDynamics.h"
#include "DySolverConstraint1D.h"
#include "PxvSimStats.h"
#include "DyTGSContactPrep.h"
#include "DyFeatherstoneArticulation.h"
#include "DySleep.h"
#include "DyTGS.h"
#include "DyResidualAccumulator.h"
#include "DyThreadContext.h"
#include "DyIslandManager.h"

#define PX_USE_BLOCK_SOLVER 1
#define PX_USE_BLOCK_1D 1

namespace physx
{
namespace Dy
{
	static inline void waitForBodyProgress(PxTGSSolverBodyVel& body, PxU32 desiredProgress, PxU32 iteration)
	{
		const PxI32 target = PxI32(desiredProgress + body.nbStaticInteractions * iteration);

		volatile PxI32* progress = reinterpret_cast<PxI32*>(&body.partitionMask);

		WAIT_FOR_PROGRESS(progress, target);
	}

	static inline void incrementBodyProgress(PxTGSSolverBodyVel& body)
	{
		if (body.nbStaticInteractions != 0)
			(*reinterpret_cast<volatile PxU32*>(&body.partitionMask))++;
	}

	static inline void waitForArticulationProgress(Dy::FeatherstoneArticulation& artic, PxU32 desiredProgress, PxU32 iteration)
	{
		const PxI32 target = PxI32(desiredProgress + artic.maxSolverFrictionProgress * iteration);

		volatile PxI32* progress = reinterpret_cast<PxI32*>(&artic.solverProgress);

		WAIT_FOR_PROGRESS(progress, target);
	}

	static inline void incrementArticulationProgress(Dy::FeatherstoneArticulation& artic)
	{
		(*reinterpret_cast<volatile PxU32*>(&artic.solverProgress))++;
	}

	static inline void waitForProgresses(const PxSolverConstraintDesc& desc, PxU32 iteration)
	{
		if (desc.linkIndexA == PxSolverConstraintDesc::RIGID_BODY)
			waitForBodyProgress(*desc.tgsBodyA, desc.progressA, iteration);
		else
			waitForArticulationProgress(*getArticulationA(desc), desc.progressA, iteration);
		if (desc.linkIndexB == PxSolverConstraintDesc::RIGID_BODY)
			waitForBodyProgress(*desc.tgsBodyB, desc.progressB, iteration);
		else
			waitForArticulationProgress(*getArticulationB(desc), desc.progressB, iteration);
	}

	static inline void incrementProgress(const PxSolverConstraintDesc& desc)
	{
		if (desc.linkIndexA == PxSolverConstraintDesc::RIGID_BODY)
		{
			incrementBodyProgress(*desc.tgsBodyA);
		}
		else
			incrementArticulationProgress(*getArticulationA(desc));
		if (desc.linkIndexB == PxSolverConstraintDesc::RIGID_BODY)
			incrementBodyProgress(*desc.tgsBodyB);
		else if(desc.articulationA != desc.articulationB)
			incrementArticulationProgress(*getArticulationB(desc));
	}

Context* createTGSDynamicsContext(	PxcNpMemBlockPool* memBlockPool, PxcScratchAllocator& scratchAllocator, Cm::FlushPool& taskPool,
									PxvSimStats& simStats, PxTaskManager* taskManager, PxVirtualAllocatorCallback* allocatorCallback,
									PxsMaterialManager* materialManager, IG::SimpleIslandManager& islandManager, PxU64 contextID,
									bool enableStabilization, bool useEnhancedDeterminism,
									PxReal lengthScale, bool externalForcesEveryTgsIterationEnabled, bool isResidualReportingEnabled)
{
	return PX_NEW(DynamicsTGSContext)(	memBlockPool, scratchAllocator, taskPool, simStats, taskManager, allocatorCallback, materialManager, islandManager, contextID,
										enableStabilization, useEnhancedDeterminism, lengthScale, externalForcesEveryTgsIterationEnabled, isResidualReportingEnabled);
}

void DynamicsTGSContext::destroy()
{
	this->~DynamicsTGSContext();
	PX_FREE_THIS;
}

PX_FORCE_INLINE PxVec3 safeRecip(const PxVec3& v)
{
	return PxVec3(v.x == 0.f ? 0.f : 1.f/v.x, v.y == 0.f ? 0.f : 1.f/v.y, v.z == 0.f ? 0.f : 1.f/v.z);
}

void copyToSolverBodyDataStep(const PxVec3& linearVelocity, const PxVec3& angularVelocity, PxReal invMass, const PxVec3& invInertia, const PxTransform& globalPose,
	PxReal maxDepenetrationVelocity, PxReal maxContactImpulse, PxU32 nodeIndex, PxReal reportThreshold, 
	PxReal maxAngVelSq, PxU32 lockFlags, bool isKinematic,
	PxTGSSolverBodyVel& solverVel, PxTGSSolverBodyTxInertia& solverBodyTxInertia, PxTGSSolverBodyData& solverBodyData, PxReal dt,
	bool gyroscopicForces)
{
	const PxMat33Padded rotation(globalPose.q);

	const PxVec3 sqrtInvInertia = computeSafeSqrtInertia(invInertia);

	const PxVec3 sqrtBodySpaceInertia = safeRecip(sqrtInvInertia);

	Cm::transformInertiaTensor(sqrtInvInertia, rotation, solverBodyTxInertia.sqrtInvInertia);	

	solverBodyTxInertia.body2WorldP = globalPose.p;
	solverBodyTxInertia.deltaBody2WorldQ = PxQuat(PxIdentity);

	PxMat33 sqrtInertia;
	Cm::transformInertiaTensor(sqrtBodySpaceInertia, rotation, sqrtInertia);

	PxVec3 lv = linearVelocity;
	PxVec3 av = angularVelocity;

	if (gyroscopicForces)
	{
		const PxVec3 localInertia(
			invInertia.x == 0.f ? 0.f : 1.f / invInertia.x,
			invInertia.y == 0.f ? 0.f : 1.f / invInertia.y,
			invInertia.z == 0.f ? 0.f : 1.f / invInertia.z);

		PxVec3 localAngVel = globalPose.q.rotateInv(av);
		PxVec3 origMom = localInertia.multiply(localAngVel);
		PxVec3 torque = -localAngVel.cross(origMom);
		PxVec3 newMom = origMom + torque * dt;
		const PxReal denom = newMom.magnitude();
		PxReal ratio = denom > 0.f ? origMom.magnitude() / denom : 0.f;
		newMom *= ratio;
		PxVec3 newDeltaAngVel = globalPose.q.rotate(invInertia.multiply(newMom) - localAngVel);
		av += newDeltaAngVel;
	}

	if (lockFlags)
	{
		if (lockFlags & PxRigidDynamicLockFlag::eLOCK_LINEAR_X)
			lv.x = 0.f;
		if (lockFlags & PxRigidDynamicLockFlag::eLOCK_LINEAR_Y)
			lv.y = 0.f;
		if (lockFlags & PxRigidDynamicLockFlag::eLOCK_LINEAR_Z)
			lv.z = 0.f;

		//KS - technically, we can zero the inertia columns and produce stiffer constraints. However, this can cause numerical issues with the 
		//joint solver, which is fixed by disabling joint preprocessing and setting minResponseThreshold to some reasonable value > 0. However, until
		//this is handled automatically, it's probably better not to zero these inertia rows
		if (lockFlags & PxRigidDynamicLockFlag::eLOCK_ANGULAR_X)
		{
			av.x = 0.f;
			//data.sqrtInvInertia.column0 = PxVec3(0.f);
		}
		if (lockFlags & PxRigidDynamicLockFlag::eLOCK_ANGULAR_Y)
		{
			av.y = 0.f;
			//data.sqrtInvInertia.column1 = PxVec3(0.f);
		}
		if (lockFlags & PxRigidDynamicLockFlag::eLOCK_ANGULAR_Z)
		{
			av.z = 0.f;
			//data.sqrtInvInertia.column2 = PxVec3(0.f);
		}
	}

	solverVel.linearVelocity = lv;
	solverVel.angularVelocity = sqrtInertia * av;
	solverVel.deltaLinDt = PxVec3(0.f);
	solverVel.deltaAngDt = PxVec3(0.f);
	solverVel.lockFlags = PxU16(lockFlags);
	solverVel.isKinematic = isKinematic;
	solverVel.maxAngVel = PxSqrt(maxAngVelSq);
	solverVel.partitionMask = 0;

	solverBodyData.nodeIndex = nodeIndex;
	solverBodyData.invMass = invMass;
	solverBodyData.penBiasClamp = maxDepenetrationVelocity;
	solverBodyData.maxContactImpulse = maxContactImpulse;	
	solverBodyData.reportThreshold = reportThreshold;
	solverBodyData.originalLinearVelocity = lv;
	solverBodyData.originalAngularVelocity = av;

	PX_ASSERT(lv.isFinite());
	PX_ASSERT(av.isFinite());
}

void copyToSolverBodyDataStepKinematic(const PxVec3& linearVelocity, const PxVec3& angularVelocity, const PxTransform& globalPose,
	PxReal maxDepenetrationVelocity, PxReal maxContactImpulse, PxU32 nodeIndex, PxReal reportThreshold, PxReal maxAngVelSq,
	PxTGSSolverBodyVel& solverVel, PxTGSSolverBodyTxInertia& solverBodyTxInertia, PxTGSSolverBodyData& solverBodyData)
{
	const PxMat33Padded rotation(globalPose.q);

	solverBodyTxInertia.body2WorldP = globalPose.p;
	solverBodyTxInertia.deltaBody2WorldQ = PxQuat(PxIdentity);

	solverBodyTxInertia.sqrtInvInertia = PxMat33(PxVec3(0.f), PxVec3(0.f), PxVec3(0.f));

	solverVel.linearVelocity = PxVec3(0.f);
	solverVel.angularVelocity = PxVec3(0.f);
	solverVel.deltaLinDt = PxVec3(0.f);
	solverVel.deltaAngDt = PxVec3(0.f);
	solverVel.lockFlags = 0;
	solverVel.isKinematic = true;
	solverVel.maxAngVel = PxSqrt(maxAngVelSq);
	solverVel.partitionMask = 0;
	solverVel.maxDynamicPartition = 0;
	solverVel.nbStaticInteractions = 0;

	solverBodyData.nodeIndex = nodeIndex;
	solverBodyData.invMass = 0.f;
	solverBodyData.penBiasClamp = maxDepenetrationVelocity;
	solverBodyData.maxContactImpulse = maxContactImpulse;
	solverBodyData.reportThreshold = reportThreshold;
	solverBodyData.originalLinearVelocity = linearVelocity;
	solverBodyData.originalAngularVelocity = angularVelocity;
}

DynamicsTGSContext::DynamicsTGSContext(	PxcNpMemBlockPool* memBlockPool,
										PxcScratchAllocator& /*scratchAllocator*/,
										Cm::FlushPool& taskPool,
										PxvSimStats& simStats,
										PxTaskManager* /*taskManager*/,
										PxVirtualAllocatorCallback* allocatorCallback,
										PxsMaterialManager* materialManager,
										IG::SimpleIslandManager& islandManager,
										PxU64 contextID,
										bool enableStabilization,
										bool useEnhancedDeterminism,
										PxReal lengthScale,
										bool isExternalForcesEveryTgsIterationEnabled,
										bool isResidualReportingEnabled) :
	DynamicsContextBase	(memBlockPool, taskPool, simStats, allocatorCallback, materialManager, islandManager, contextID, PX_MAX_F32, lengthScale, enableStabilization, useEnhancedDeterminism, isResidualReportingEnabled),
	mIsExternalForcesEveryTgsIterationEnabled(isExternalForcesEveryTgsIterationEnabled)
{
	createThresholdStream(*allocatorCallback);
	createForceChangeThresholdStream(*allocatorCallback);
	mExceededForceThresholdStream[0] = PX_NEW(ThresholdStream)(*allocatorCallback);
	mExceededForceThresholdStream[1] = PX_NEW(ThresholdStream)(*allocatorCallback);

	PxMemZero(&mWorldSolverBodyVel, sizeof(mWorldSolverBodyVel));
	
	mWorldSolverBodyVel.lockFlags = 0;
	mWorldSolverBodyVel.isKinematic = false;
	mWorldSolverBodyTxInertia.sqrtInvInertia = PxMat33(PxZero);
	mWorldSolverBodyTxInertia.body2WorldP = PxVec3(PxZero);
	mWorldSolverBodyTxInertia.deltaBody2WorldQ = PxQuat(PxIdentity);	
	mWorldSolverBodyData2.penBiasClamp = -PX_MAX_REAL;
	mWorldSolverBodyData2.maxContactImpulse = PX_MAX_REAL;
	mWorldSolverBodyData2.nodeIndex = PX_INVALID_NODE;
	mWorldSolverBodyData2.invMass = 0;
	mWorldSolverBodyData2.reportThreshold = PX_MAX_REAL;
	mWorldSolverBodyData2.originalLinearVelocity = PxVec3(0.f);
	mWorldSolverBodyData2.originalAngularVelocity = PxVec3(0.f);
}

DynamicsTGSContext::~DynamicsTGSContext()
{
	PX_DELETE(mExceededForceThresholdStream[1]);
	PX_DELETE(mExceededForceThresholdStream[0]);
}

PX_COMPILE_TIME_ASSERT(PxsIndexedInteraction::eBODY == 0);
PX_COMPILE_TIME_ASSERT(PxsIndexedInteraction::eKINEMATIC == 1);

void DynamicsTGSContext::setDescFromIndices_Contacts(PxSolverConstraintDesc& desc, const IG::IslandSim& islandSim,
	const PxsIndexedInteraction& constraint, PxU32 solverBodyOffset, PxTGSSolverBodyVel* solverBodies)
{
	const PxU32 offsetMap[] = { solverBodyOffset, 0 };
	//const PxU32 offsetMap[] = {mKinematicCount, 0};

	if (constraint.indexType0 == PxsIndexedInteraction::eARTICULATION)
	{
		const PxNodeIndex nodeIndex0(constraint.articulation0);
		const IG::Node& node0 = islandSim.getNode(nodeIndex0);
		desc.articulationA = node0.mObject;
		desc.linkIndexA = nodeIndex0.articulationLinkId();
		desc.bodyADataIndex = 0;
	}
	else
	{
		desc.tgsBodyA = constraint.indexType0 == PxsIndexedInteraction::eWORLD ? &mWorldSolverBodyVel
			: &solverBodies[PxU32(constraint.solverBody0) + offsetMap[constraint.indexType0] + 1];

		desc.bodyADataIndex = constraint.indexType0 == PxsIndexedInteraction::eWORLD ? 0
			: PxU32(constraint.solverBody0) + offsetMap[constraint.indexType0] + 1;

		desc.linkIndexA = PxSolverConstraintDesc::RIGID_BODY;
	}

	if (constraint.indexType1 == PxsIndexedInteraction::eARTICULATION)
	{
		const PxNodeIndex nodeIndex1(constraint.articulation1);
		const IG::Node& node1 = islandSim.getNode(nodeIndex1);
		desc.articulationB = node1.mObject;
		desc.linkIndexB = nodeIndex1.articulationLinkId();// PxTo8(getLinkIndex(constraint.articulation1));
		desc.bodyBDataIndex = 0;
	}
	else
	{
		desc.tgsBodyB = constraint.indexType1 == PxsIndexedInteraction::eWORLD ? &mWorldSolverBodyVel
			: &solverBodies[PxU32(constraint.solverBody1) + offsetMap[constraint.indexType1] + 1];

		desc.bodyBDataIndex = constraint.indexType1 == PxsIndexedInteraction::eWORLD ? 0
			: PxU32(constraint.solverBody1) + offsetMap[constraint.indexType1] + 1;

		desc.linkIndexB = PxSolverConstraintDesc::RIGID_BODY;
	}
}

void DynamicsTGSContext::setDescFromIndices_Constraints(PxSolverConstraintDesc& desc, const IG::IslandSim& islandSim, IG::EdgeIndex edgeIndex,
	const PxU32* bodyRemap, PxU32 solverBodyOffset, PxTGSSolverBodyVel* solverBodies)
{
	const PxNodeIndex node1 = islandSim.mCpuData.getNodeIndex1(edgeIndex);
	if (node1.isStaticBody())
	{
		desc.tgsBodyA = &mWorldSolverBodyVel;
		desc.bodyADataIndex = 0;
		desc.linkIndexA = PxSolverConstraintDesc::RIGID_BODY;
	}
	else
	{
		const IG::Node& node = islandSim.getNode(node1);

		if (node.getNodeType() == IG::Node::eARTICULATION_TYPE)
		{
			PX_ASSERT(node1.isArticulation());
			desc.articulationA = getArticulationFromIG(islandSim, node1);
			desc.linkIndexA = node1.articulationLinkId();
			desc.bodyADataIndex = 0;
		}
		else
		{
			PX_ASSERT(!node1.isArticulation());
			const PxU32 activeIndex = islandSim.getActiveNodeIndex(node1);
			const PxU32 index = node.isKinematic() ? activeIndex : bodyRemap[activeIndex] + solverBodyOffset;
			desc.tgsBodyA = &solverBodies[index + 1];
			desc.bodyADataIndex = index + 1;
			desc.linkIndexA = PxSolverConstraintDesc::RIGID_BODY;
		}
	}

	const PxNodeIndex node2 = islandSim.mCpuData.getNodeIndex2(edgeIndex);
	if (node2.isStaticBody())
	{
		desc.tgsBodyB = &mWorldSolverBodyVel;
		desc.bodyBDataIndex = 0;
		desc.linkIndexB = PxSolverConstraintDesc::RIGID_BODY;
	}
	else
	{
		const IG::Node& node = islandSim.getNode(node2);
		if (node.getNodeType() == IG::Node::eARTICULATION_TYPE)
		{
			PX_ASSERT(node2.isArticulation());
			desc.articulationB = getArticulationFromIG(islandSim, node2);
			desc.linkIndexB = node2.articulationLinkId();
			desc.bodyBDataIndex = 0;
		}
		else
		{
			PX_ASSERT(!node2.isArticulation());
			const PxU32 activeIndex = islandSim.getActiveNodeIndex(node2);
			const PxU32 index = node.isKinematic() ? activeIndex : bodyRemap[activeIndex] + solverBodyOffset;
			desc.tgsBodyB = &solverBodies[index + 1];
			desc.bodyBDataIndex = index + 1;
			desc.linkIndexB = PxSolverConstraintDesc::RIGID_BODY;
		}
	}
}

namespace
{
class DynamicsMergeTask : public Cm::Task 
{
	PxBaseTask* mSecondContinuation;

public:
	DynamicsMergeTask(PxU64 contextId) : Cm::Task(contextId), mSecondContinuation(NULL)
	{
	}

	void setSecondContinuation(PxBaseTask* task) { task->addReference();  mSecondContinuation = task; }

	virtual const char* getName() const { return "MergeTask"; }

	virtual void runInternal()
	{
	}

	virtual void release()
	{
		mSecondContinuation->removeReference();
		Cm::Task::release();
	}
};

class KinematicCopyTGSTask : public Cm::Task 
{
	const PxNodeIndex* const mKinematicIndices;
	const PxU32 mNbKinematics;
	const IG::IslandSim& mIslandSim;
	PxTGSSolverBodyVel* mVels;
	PxTGSSolverBodyTxInertia* mInertia;
	PxTGSSolverBodyData* mBodyData;

	PX_NOCOPY(KinematicCopyTGSTask)

public:

	static const PxU32 NbKinematicsPerTask = 1024;

	KinematicCopyTGSTask(const PxNodeIndex* const kinematicIndices,
		PxU32 nbKinematics, const IG::IslandSim& islandSim, PxTGSSolverBodyVel* vels,
		PxTGSSolverBodyTxInertia* inertias, PxTGSSolverBodyData* datas, PxU64 contextID) : Cm::Task(contextID), 
		mKinematicIndices(kinematicIndices), mNbKinematics(nbKinematics),
		mIslandSim(islandSim), mVels(vels), mInertia(inertias), mBodyData(datas)
	{
	}

	virtual const char* getName() const { return "KinematicCopyTask"; }

	virtual void runInternal()
	{
		for (PxU32 i = 0; i<mNbKinematics; i++)
		{
			PxsRigidBody* rigidBody = getRigidBodyFromIG(mIslandSim, mKinematicIndices[i]);
			const PxsBodyCore& core = rigidBody->getCore();
			copyToSolverBodyDataStepKinematic(core.linearVelocity, core.angularVelocity, core.body2World, core.maxPenBias,
				core.maxContactImpulse, mKinematicIndices[i].index(), core.contactReportThreshold, core.maxAngularVelocitySq,
				mVels[i], mInertia[i], mBodyData[i]);

			rigidBody->saveLastCCDTransform();
		}
	}
};

class UpdateContinuationTGSTask : public Cm::Task
{
	DynamicsTGSContext& mContext;
	IG::SimpleIslandManager& mSimpleIslandManager;
	PxBaseTask* mLostTouchTask;
	PxU32 mMaxArticulationLinks;

	PX_NOCOPY(UpdateContinuationTGSTask)
public:

	UpdateContinuationTGSTask(DynamicsTGSContext& context,
	IG::SimpleIslandManager& simpleIslandManager,
	PxBaseTask* lostTouchTask, PxU64 contextID, PxU32 maxLinks) :
		Cm::Task(contextID), mContext(context), mSimpleIslandManager(simpleIslandManager),
		mLostTouchTask(lostTouchTask), mMaxArticulationLinks(maxLinks)
	{
	}

	virtual const char* getName() const { return "UpdateContinuationTask";}

	virtual void runInternal()
	{
		mContext.updatePostKinematic(mSimpleIslandManager, mCont, mLostTouchTask, mMaxArticulationLinks);
		//Allow lost touch task to run once all tasks have be scheduled
		mLostTouchTask->removeReference();
	}
};
}

void DynamicsTGSContext::update(Cm::FlushPool& /*flushPool*/, PxBaseTask* continuation, PxBaseTask* /*postPartitioningTask*/, PxBaseTask* lostTouchTask,
	PxvNphaseImplementationContext* nphase, PxU32 /*maxPatchesPerCM*/, PxU32 maxArticulationLinks,
	PxReal dt, const PxVec3& gravity, PxBitMapPinned& /*changedHandleMap*/)
{
	PX_PROFILE_ZONE("Dynamics.solverQueueTasks", mContextID);

	mOutputIterator = nphase->getContactManagerOutputs();

	// PT: TODO: refactor
	mDt = dt;
	mInvDt = 1.0f / dt;
	mGravity = gravity;

	const IG::IslandSim& islandSim = mIslandManager.getAccurateIslandSim();

	const PxU32 islandCount = islandSim.getNbActiveIslands();

	const PxU32 activatedContactCount = islandSim.getNbActivatedEdges(IG::Edge::eCONTACT_MANAGER);
	const IG::EdgeIndex* const activatingEdges = islandSim.getActivatedEdges(IG::Edge::eCONTACT_MANAGER);

	{
		PX_PROFILE_ZONE("resetFrictionPatchCount", mContextID);

		for (PxU32 a = 0; a < activatedContactCount; ++a)
		{
			PxsContactManager* cm = mIslandManager.getContactManager(activatingEdges[a]);
			if (cm)
				cm->getWorkUnit().mFrictionPatchCount = 0; //KS - zero the friction patch count on any activating edges
		}
	}

#if PX_ENABLE_SIM_STATS
	if (islandCount > 0)
	{
		mSimStats.mNbActiveKinematicBodies = islandSim.getNbActiveKinematics();
		mSimStats.mNbActiveDynamicBodies = islandSim.getNbActiveNodes(IG::Node::eRIGID_BODY_TYPE);
		mSimStats.mNbActiveConstraints = islandSim.getNbActiveEdges(IG::Edge::eCONSTRAINT);
	}
	else
	{
		mSimStats.mNbActiveKinematicBodies = islandSim.getNbActiveKinematics();
		mSimStats.mNbActiveDynamicBodies = 0;
		mSimStats.mNbActiveConstraints = 0;
	}
#else
	PX_CATCH_UNDEFINED_ENABLE_SIM_STATS
#endif

	mThresholdStreamOut = 0;

	resetThreadContexts();

	//If there is no work to do then we can do nothing at all.
	if(!islandCount)
		return;

	//Block to make sure it doesn't run before stage2 of update!
	lostTouchTask->addReference();

	UpdateContinuationTGSTask* task = PX_PLACEMENT_NEW(mTaskPool.allocate(sizeof(UpdateContinuationTGSTask)), UpdateContinuationTGSTask)
		(*this, mIslandManager, lostTouchTask, mContextID, maxArticulationLinks);

	task->setContinuation(continuation);

	//KS - test that world solver body's velocities are finite and 0, then set it to 0.
	//Technically, the velocity should always be 0 but can be stomped if a NAN creeps into the simulation.
	PX_ASSERT(mWorldSolverBodyVel.linearVelocity == PxVec3(0.f));
	PX_ASSERT(mWorldSolverBodyVel.angularVelocity == PxVec3(0.f));
	PX_ASSERT(mWorldSolverBodyVel.linearVelocity.isFinite());
	PX_ASSERT(mWorldSolverBodyVel.angularVelocity.isFinite());

	mWorldSolverBodyVel.linearVelocity = mWorldSolverBodyVel.angularVelocity = PxVec3(0.f);

	const PxU32 kinematicCount = islandSim.getNbActiveKinematics();
	const PxNodeIndex* const kinematicIndices = islandSim.getActiveKinematics();
	mKinematicCount = kinematicCount;

	{
		const PxU32 bodyCount = islandSim.getNbActiveNodes(IG::Node::eRIGID_BODY_TYPE);

		if (kinematicCount + bodyCount > mSolverBodyVelPool.capacity())
		{
			PX_PROFILE_ZONE("reserve", mContextID);

			mSolverBodyRemapTable.reserve((kinematicCount + bodyCount + 31 + 1) & ~31);
			mSolverBodyVelPool.reserve((kinematicCount + bodyCount +31 + 1) & ~31);
			mSolverBodyTxInertiaPool.reserve((kinematicCount + bodyCount +31 + 1) & ~31);
			mSolverBodyDataPool2.reserve((kinematicCount + bodyCount +31 + 1) & ~31);
		}

		{
			PX_PROFILE_ZONE("resize", mContextID);

			mSolverBodyVelPool.resize(kinematicCount + bodyCount +1);
			mSolverBodyTxInertiaPool.resize(kinematicCount + bodyCount +1);
			mSolverBodyDataPool2.resize(kinematicCount + bodyCount +1);
			mSolverBodyRemapTable.resize(kinematicCount + bodyCount + 1);
		}

		// integrate and copy all the kinematics - overkill, since not all kinematics
		// need solver bodies

		mSolverBodyVelPool[0] = mWorldSolverBodyVel;
		mSolverBodyTxInertiaPool[0] = mWorldSolverBodyTxInertia;
		mSolverBodyDataPool2[0] = mWorldSolverBodyData2;

		if(kinematicCount)
		{
			PX_PROFILE_ZONE("Dynamics.updateKinematics", mContextID);

			// PT: TODO: why no PxMemZero here compared to PGS?
			for (PxU32 i = 0; i < kinematicCount; i+= KinematicCopyTGSTask::NbKinematicsPerTask)
			{
				const PxU32 nbToProcess = PxMin(kinematicCount - i, KinematicCopyTGSTask::NbKinematicsPerTask);

				KinematicCopyTGSTask* kinematicTask = PX_PLACEMENT_NEW(mTaskPool.allocate(sizeof(KinematicCopyTGSTask)), KinematicCopyTGSTask)
					(kinematicIndices + i, nbToProcess, islandSim, &mSolverBodyVelPool[i+1],
						&mSolverBodyTxInertiaPool[i + 1], &mSolverBodyDataPool2[i + 1], mContextID);

				kinematicTask->setContinuation(task);
				kinematicTask->removeReference();
			}
		}
	}

	{
		PX_PROFILE_ZONE("reserve2", mContextID);

		const PxU32 totalConstraintCount = reserveSharedSolverConstraintsArrays(islandSim, maxArticulationLinks);

		mSolverConstraintDescPool.forceSize_Unsafe(0);
		mSolverConstraintDescPool.reserve((totalConstraintCount + 63) & (~63));
		mSolverConstraintDescPool.forceSize_Unsafe(totalConstraintCount);

		mOrderedSolverConstraintDescPool.forceSize_Unsafe(0);
		mOrderedSolverConstraintDescPool.reserve((totalConstraintCount + 63) & (~63));
		mOrderedSolverConstraintDescPool.forceSize_Unsafe(totalConstraintCount);

		mTempSolverConstraintDescPool.forceSize_Unsafe(0);
		mTempSolverConstraintDescPool.reserve((totalConstraintCount + 63) & (~63));
		mTempSolverConstraintDescPool.forceSize_Unsafe(totalConstraintCount);
	}

	task->removeReference();
}

void DynamicsTGSContext::updatePostKinematic(IG::SimpleIslandManager& simpleIslandManager, PxBaseTask* continuation, PxBaseTask* lostTouchTask, PxU32 maxLinks)
{
	const IG::IslandSim& islandSim = simpleIslandManager.getAccurateIslandSim();

	const IG::IslandId*const islandIds = islandSim.getActiveIslands();

	DynamicsMergeTask* mergeTask = PX_PLACEMENT_NEW(mTaskPool.allocate(sizeof(DynamicsMergeTask)), DynamicsMergeTask)(mContextID);

	mergeTask->setContinuation(continuation);
	mergeTask->setSecondContinuation(lostTouchTask);

	PxU32 currentIsland = 0;
	PxU32 currentBodyIndex = 0;
	PxU32 currentArticulation = 0;
	PxU32 currentContact = 0;

	PxU32 constraintIndex = 0;

	const PxU32 minIslandSize = mSolverBatchSize;

	const PxU32 islandCount = islandSim.getNbActiveIslands();

	const PxU32 articulationBatchSize = mSolverArticBatchSize;

	while (currentIsland < islandCount)
	{
		SolverIslandObjectsStep objectStarts;
		objectStarts.articulations			= mArticulationArray.begin() + currentArticulation;
		objectStarts.bodies					= mRigidBodyArray.begin() + currentBodyIndex;
		objectStarts.externalAccelerations	= &mRigidExternalAccelerations;
		objectStarts.contactManagers		= mContactList.begin() + currentContact;
		objectStarts.constraintDescs		= mSolverConstraintDescPool.begin() + constraintIndex;
		objectStarts.orderedConstraintDescs	= mOrderedSolverConstraintDescPool.begin() + constraintIndex;
		objectStarts.constraintBatchHeaders	= mContactConstraintBatchHeaders.begin() + constraintIndex;
		objectStarts.tempConstraintDescs	= mTempSolverConstraintDescPool.begin() + constraintIndex;
		objectStarts.motionVelocities		= mMotionVelocityArray.begin() + currentBodyIndex;
		objectStarts.bodyCoreArray			= mBodyCoreArray.begin() + currentBodyIndex;
		objectStarts.islandIds				= islandIds + currentIsland;
		objectStarts.bodyRemapTable			= mSolverBodyRemapTable.begin();
		objectStarts.nodeIndexArray			= mNodeIndexArray.begin() + currentBodyIndex;

		PxU32 startIsland = currentIsland;
		PxU32 constraintCount = 0;

		PxU32 nbArticulations = 0;
		PxU32 nbBodies = 0;
		PxU32 nbConstraints = 0;
		PxU32 nbContactManagers = 0;

		while (nbBodies < minIslandSize && currentIsland < islandCount && nbArticulations < articulationBatchSize)
		{
			const IG::Island& island = islandSim.getIsland(islandIds[currentIsland]);

			nbBodies += island.mNodeCount[IG::Node::eRIGID_BODY_TYPE];
			nbArticulations += island.mNodeCount[IG::Node::eARTICULATION_TYPE];
			nbConstraints += island.mEdgeCount[IG::Edge::eCONSTRAINT];
			nbContactManagers += island.mEdgeCount[IG::Edge::eCONTACT_MANAGER];
			constraintCount = nbConstraints + nbContactManagers;
			currentIsland++;
		}

		objectStarts.numIslands = currentIsland - startIsland;

		constraintIndex += nbArticulations* maxLinks;

		PxsIslandIndices counts;

		counts.articulations = nbArticulations;
		counts.bodies = nbBodies;

		counts.constraints = nbConstraints;
		counts.contactManagers = nbContactManagers;
		
		solveIsland(objectStarts, counts,
			mKinematicCount + currentBodyIndex, simpleIslandManager, mSolverBodyRemapTable.begin(), mMaterialManager, mOutputIterator,
			mergeTask);

		currentBodyIndex += nbBodies;
		currentArticulation += nbArticulations;
		currentContact += nbContactManagers;

		constraintIndex += constraintCount;
	}

	mergeTask->removeReference();
}

void DynamicsTGSContext::prepareBodiesAndConstraints(const SolverIslandObjectsStep& objects,
	IG::SimpleIslandManager& islandManager, IslandContextStep& islandContext)
{
	Dy::ThreadContext& mThreadContext = *islandContext.mThreadContext;

	mThreadContext.mMaxSolverPositionIterations = 0;
	mThreadContext.mMaxSolverVelocityIterations = 0;
	mThreadContext.mAxisConstraintCount = 0;
	mThreadContext.mContactDescPtr = mThreadContext.contactConstraintDescArray;
	mThreadContext.mNumDifferentBodyConstraints = 0;
	mThreadContext.mNumStaticConstraints = 0;
	mThreadContext.numContactConstraintBatches = 0;
	mThreadContext.contactDescArraySize = 0;

	mThreadContext.motionVelocityArray = objects.motionVelocities;
	mThreadContext.mBodyCoreArray = objects.bodyCoreArray;
	mThreadContext.mRigidBodyArray = objects.bodies;
	mThreadContext.mArticulationArray = objects.articulations;
	mThreadContext.bodyRemapTable = objects.bodyRemapTable;
	mThreadContext.mNodeIndexArray = objects.nodeIndexArray;

	mThreadContext.resizeArrays(islandContext.mCounts.articulations);

	PxsBodyCore** PX_RESTRICT bodyArrayPtr = mThreadContext.mBodyCoreArray;
	PxsRigidBody** PX_RESTRICT rigidBodyPtr = mThreadContext.mRigidBodyArray;
	FeatherstoneArticulation** PX_RESTRICT articulationPtr = mThreadContext.mArticulationArray;
	PxU32* PX_RESTRICT bodyRemapTable = mThreadContext.bodyRemapTable;
	PxU32* PX_RESTRICT nodeIndexArray = mThreadContext.mNodeIndexArray;

	PxU32 nbIslands = objects.numIslands;
	const IG::IslandId* const islandIds = objects.islandIds;

	const IG::IslandSim& islandSim = islandManager.getAccurateIslandSim();

	{
		PX_PROFILE_ZONE("IterateIslandsNodes", mContextID);

		PxU32 bodyIndex = 0, articIndex = 0;
		for (PxU32 i = 0; i < nbIslands; ++i)
		{
			const IG::Island& island = islandSim.getIsland(islandIds[i]);

			PxNodeIndex currentIndex = island.mRootNode;

			while (currentIndex.isValid())
			{
				const IG::Node& node = islandSim.getNode(currentIndex);

				if (node.getNodeType() == IG::Node::eARTICULATION_TYPE)
				{
					articulationPtr[articIndex++] = getObjectFromIG<FeatherstoneArticulation>(node);
				}
				else
				{
					PxsRigidBody* rigid = getObjectFromIG<PxsRigidBody>(node);
					PX_ASSERT(bodyIndex < (islandContext.mCounts.bodies + mKinematicCount + 1));
					rigidBodyPtr[bodyIndex] = rigid;
					bodyArrayPtr[bodyIndex] = &rigid->getCore();
					nodeIndexArray[bodyIndex] = currentIndex.index();
					bodyRemapTable[islandSim.getActiveNodeIndex(currentIndex)] = bodyIndex++;
				}

				currentIndex = node.mNextNode;
			}
		}
	}

	{
		PX_PROFILE_ZONE("IterateIslandsContactEdges", mContextID);

		PxsIndexedContactManager* indexedManagers = objects.contactManagers;

		PxU32 currentContactIndex = 0;
		for (PxU32 i = 0; i < nbIslands; ++i)
		{
			const IG::Island& island = islandSim.getIsland(islandIds[i]);

			IG::EdgeIndex contactEdgeIndex = island.mFirstEdge[IG::Edge::eCONTACT_MANAGER];

			while (contactEdgeIndex != IG_INVALID_EDGE)
			{
				const IG::Edge& edge = islandSim.getEdge(contactEdgeIndex);

				PxsContactManager* contactManager = islandManager.getContactManager(contactEdgeIndex);

				if (contactManager)
				{
					const PxNodeIndex nodeIndex1 = islandSim.mCpuData.getNodeIndex1(contactEdgeIndex);
					const PxNodeIndex nodeIndex2 = islandSim.mCpuData.getNodeIndex2(contactEdgeIndex);

					PxsIndexedContactManager& indexedManager = indexedManagers[currentContactIndex++];
					indexedManager.contactManager = contactManager;

					PX_ASSERT(!nodeIndex1.isStaticBody());
					{
						const IG::Node& node1 = islandSim.getNode(nodeIndex1);

						//Is it an articulation or not???
						if (node1.getNodeType() == IG::Node::eARTICULATION_TYPE)
						{
							indexedManager.articulation0 = nodeIndex1.getInd();
							indexedManager.indexType0 = PxsIndexedInteraction::eARTICULATION;
						}
						else
						{
							if (node1.isKinematic())
							{
								indexedManager.indexType0 = PxsIndexedInteraction::eKINEMATIC;
								indexedManager.solverBody0 = islandSim.getActiveNodeIndex(nodeIndex1);
							}
							else
							{
								indexedManager.indexType0 = PxsIndexedInteraction::eBODY;
								indexedManager.solverBody0 = bodyRemapTable[islandSim.getActiveNodeIndex(nodeIndex1)];
							}
							PX_ASSERT(indexedManager.solverBody0 < (islandContext.mCounts.bodies + mKinematicCount + 1));
						}
					}

					if (nodeIndex2.isStaticBody())
					{
						indexedManager.indexType1 = PxsIndexedInteraction::eWORLD;
					}
					else
					{
						const IG::Node& node2 = islandSim.getNode(nodeIndex2);

						//Is it an articulation or not???
						if (node2.getNodeType() == IG::Node::eARTICULATION_TYPE)
						{
							indexedManager.articulation1 = nodeIndex2.getInd();
							indexedManager.indexType1 = PxsIndexedInteraction::eARTICULATION;
						}
						else
						{
							if (node2.isKinematic())
							{
								indexedManager.indexType1 = PxsIndexedInteraction::eKINEMATIC;
								indexedManager.solverBody1 = islandSim.getActiveNodeIndex(nodeIndex2);
							}
							else
							{
								indexedManager.indexType1 = PxsIndexedInteraction::eBODY;
								indexedManager.solverBody1 = bodyRemapTable[islandSim.getActiveNodeIndex(nodeIndex2)];
							}
							PX_ASSERT(indexedManager.solverBody1 < (islandContext.mCounts.bodies + mKinematicCount + 1));
						}
					}

				}
				contactEdgeIndex = edge.mNextIslandEdge;
			}
		}

		islandContext.mCounts.contactManagers = currentContactIndex;
	}
}

struct ConstraintLess
{
	bool operator()(const PxSolverConstraintDesc& left, const PxSolverConstraintDesc& right) const
	{
		return reinterpret_cast<Constraint*>(left.constraint)->index > reinterpret_cast<Constraint*>(right.constraint)->index;
	}
};

void DynamicsTGSContext::setupDescs(IslandContextStep& mIslandContext, const SolverIslandObjectsStep& mObjects,
	PxU32* mBodyRemapTable, PxU32 mSolverBodyOffset, PxsContactManagerOutputIterator& /*outputs*/)
{
	PX_PROFILE_ZONE("setupDescs", mContextID);

	ThreadContext& mThreadContext = *mIslandContext.mThreadContext;
	PxSolverConstraintDesc* contactDescPtr = mObjects.constraintDescs;

	//PxU32 constraintCount = mCounts.constraints + mCounts.contactManagers;

	PxU32 nbIslands = mObjects.numIslands;
	const IG::IslandId* const islandIds = mObjects.islandIds;

	const IG::IslandSim& islandSim = mIslandManager.getAccurateIslandSim();

	{
		PX_PROFILE_ZONE("IterateIslandsConstraintEdges", mContextID);

		for (PxU32 i = 0; i < nbIslands; ++i)
		{
			const IG::Island& island = islandSim.getIsland(islandIds[i]);

			IG::EdgeIndex edgeId = island.mFirstEdge[IG::Edge::eCONSTRAINT];

			while (edgeId != IG_INVALID_EDGE)
			{
				PxSolverConstraintDesc& desc = *contactDescPtr;

				const IG::Edge& edge = islandSim.getEdge(edgeId);
				Dy::Constraint* constraint = mIslandManager.getConstraint(edgeId);
				setDescFromIndices_Constraints(desc, islandSim, edgeId, mBodyRemapTable, mSolverBodyOffset, mSolverBodyVelPool.begin());
				desc.constraint = reinterpret_cast<PxU8*>(constraint);
				desc.constraintType = DY_SC_TYPE_RB_1D;
				contactDescPtr++;
				edgeId = edge.mNextIslandEdge;
			}
		}
	}

	{
		PX_PROFILE_ZONE("PxSort", mContextID);
		PxSort(mObjects.constraintDescs, PxU32(contactDescPtr - mObjects.constraintDescs), ConstraintLess());
	}

	if (mIslandContext.mCounts.contactManagers)
	{
		for (PxU32 a = 0; a < mIslandContext.mCounts.contactManagers; ++a)
		{
			//PxsContactManagerOutput& output = outputs.getContactManager(mObjects.contactManagers[a].contactManager->getWorkUnit().mNpIndex);
			//if (output.nbContacts > 0)
			{
				PxSolverConstraintDesc& desc = *contactDescPtr;
				setDescFromIndices_Contacts(desc, islandSim, mObjects.contactManagers[a], mSolverBodyOffset, mSolverBodyVelPool.begin());
				desc.constraint = reinterpret_cast<PxU8*>(mObjects.contactManagers[a].contactManager);
				desc.constraintType = DY_SC_TYPE_RB_CONTACT;
				contactDescPtr++;
			}
			//else
			//{
			//	//Clear friction state!
			//	mObjects.contactManagers[a].contactManager->getWorkUnit().frictionDataPtr = NULL;
			//	mObjects.contactManagers[a].contactManager->getWorkUnit().frictionPatchCount = 0;
			//}
		}
	}
	mThreadContext.contactDescArraySize = PxU32(contactDescPtr - mObjects.constraintDescs);
}

void DynamicsTGSContext::preIntegrateBodies(PxsBodyCore** bodyArray, PxsRigidBody** originalBodyArray,
	PxTGSSolverBodyVel* solverBodyVelPool, PxTGSSolverBodyTxInertia* solverBodyTxInertia, PxTGSSolverBodyData* solverBodyDataPool2, PxU32* nodeIndexArray, PxU32 bodyCount, const PxVec3& gravity, PxReal dt, PxU32& posIters, PxU32& velIters, PxU32 /*iteration*/)
{
	PX_PROFILE_ZONE("PreIntegrate", mContextID);

	const bool skipGravity = mIsExternalForcesEveryTgsIterationEnabled;

	PxU32 localMaxPosIter = 0;
	PxU32 localMaxVelIter = 0;
	for (PxU32 i = 0; i < bodyCount; ++i)
	{
		PxsBodyCore& core = *bodyArray[i];
		const PxsRigidBody& rBody = *originalBodyArray[i];

		const PxU16 iterWord = core.solverIterationCounts;
		localMaxPosIter = PxMax<PxU32>(PxU32(iterWord & 0xff), localMaxPosIter);
		localMaxVelIter = PxMax<PxU32>(PxU32(iterWord >> 8), localMaxVelIter);

		//const Cm::SpatialVector& accel = originalBodyArray[i]->getAccelerationV();
		bodyCoreComputeUnconstrainedVelocity(gravity, dt, core.linearDamping, core.angularDamping, rBody.mAccelScale, core.maxLinearVelocitySq, core.maxAngularVelocitySq,
			core.linearVelocity, core.angularVelocity, core.disableGravity!=0 || skipGravity);

		copyToSolverBodyDataStep(core.linearVelocity, core.angularVelocity, core.inverseMass, core.inverseInertia, core.body2World, core.maxPenBias, core.maxContactImpulse, nodeIndexArray[i],
			core.contactReportThreshold, core.maxAngularVelocitySq, core.lockFlags, false,
			solverBodyVelPool[i+1], solverBodyTxInertia[i+1], solverBodyDataPool2[i+1], dt, core.mFlags & PxRigidBodyFlag::eENABLE_GYROSCOPIC_FORCES);
	}

	posIters = localMaxPosIter;
	velIters = localMaxVelIter;
}

void DynamicsTGSContext::createSolverConstraints(PxSolverConstraintDesc* contactDescPtr, PxConstraintBatchHeader* headers, PxU32 nbHeaders,
	PxsContactManagerOutputIterator& outputs, Dy::ThreadContext& islandThreadContext, Dy::ThreadContext& threadContext, PxReal stepDt, PxReal totalDt, PxReal invStepDt,
	const PxReal biasCoefficient)
{
	PX_UNUSED(totalDt);
	//PX_PROFILE_ZONE("CreateConstraints", 0);
	PxTransform idt(PxIdentity);

	BlockAllocator blockAllocator(islandThreadContext.mConstraintBlockManager, threadContext.mConstraintBlockStream, threadContext.mFrictionPatchStreamPair, threadContext.mConstraintSize);

	PxTGSSolverBodyTxInertia* txInertias = mSolverBodyTxInertiaPool.begin();
	PxTGSSolverBodyData* solverBodyDatas = mSolverBodyDataPool2.begin();

	const PxReal invTotalDt = 1.0f / totalDt;

	for (PxU32 h = 0; h < nbHeaders; ++h)
	{
		PxConstraintBatchHeader& hdr = headers[h];
		PxU32 startIdx = hdr.startIndex;
		PxU32 endIdx = startIdx + hdr.stride;
		
		if (contactDescPtr[startIdx].constraintType == DY_SC_TYPE_RB_CONTACT)
		{
			PxTGSSolverContactDesc blockDescs[4];
			PxsContactManagerOutput* cmOutputs[4];
			PxsContactManager* cms[4];

			for (PxU32 a = startIdx, i = 0; a < endIdx; ++a, i++)
			{
				PxSolverConstraintDesc& desc = contactDescPtr[a];
				PxsContactManager* cm = reinterpret_cast<PxsContactManager*>(desc.constraint);

				PxTGSSolverContactDesc& blockDesc = blockDescs[i];

				cms[i] = cm;

				PxcNpWorkUnit& unit = cm->getWorkUnit();

				PxsContactManagerOutput* cmOutput = &outputs.getContactManagerOutput(unit.mNpIndex);

				cmOutputs[i] = cmOutput;

				PxTGSSolverBodyVel& b0 = *desc.tgsBodyA;
				PxTGSSolverBodyVel& b1 = *desc.tgsBodyB;

				PxTGSSolverBodyTxInertia& txI0 = txInertias[desc.bodyADataIndex];
				PxTGSSolverBodyTxInertia& txI1 = txInertias[desc.bodyBDataIndex];

				PxTGSSolverBodyData& data0 = solverBodyDatas[desc.bodyADataIndex];
				PxTGSSolverBodyData& data1 = solverBodyDatas[desc.bodyBDataIndex];

				blockDesc.body0 = &b0;
				blockDesc.body1 = &b1;
				blockDesc.bodyFrame0 = unit.mRigidCore0->body2World;
				blockDesc.bodyFrame1 = unit.mRigidCore1->body2World;
				blockDesc.shapeInteraction = cm->getShapeInteraction();
				blockDesc.contactForces = cmOutput->contactForces;
				blockDesc.desc = &desc;
				blockDesc.body0 = &b0;
				blockDesc.body1 = &b1;
				blockDesc.body0TxI = &txI0;
				blockDesc.body1TxI = &txI1;
				blockDesc.bodyData0 = &data0;
				blockDesc.bodyData1 = &data1;
				blockDesc.hasForceThresholds = !!(unit.mFlags & PxcNpWorkUnitFlag::eFORCE_THRESHOLD);
				blockDesc.disableStrongFriction = !!(unit.mFlags & PxcNpWorkUnitFlag::eDISABLE_STRONG_FRICTION);
				blockDesc.bodyState0 = (unit.mFlags & PxcNpWorkUnitFlag::eARTICULATION_BODY0) ? PxSolverContactDesc::eARTICULATION : PxSolverContactDesc::eDYNAMIC_BODY;
				if (unit.mFlags & PxcNpWorkUnitFlag::eARTICULATION_BODY1)
				{
					//kinematic link
					if (desc.linkIndexB == 0xff)
					{
						blockDesc.bodyState1 = PxSolverContactDesc::eSTATIC_BODY;
					}
					else
					{
						blockDesc.bodyState1 = PxSolverContactDesc::eARTICULATION;
					}
				}
				else
				{
					blockDesc.bodyState1 = (unit.mFlags & PxcNpWorkUnitFlag::eHAS_KINEMATIC_ACTOR) ? PxSolverContactDesc::eKINEMATIC_BODY :
						((unit.mFlags & PxcNpWorkUnitFlag::eDYNAMIC_BODY1) ? PxSolverContactDesc::eDYNAMIC_BODY : PxSolverContactDesc::eSTATIC_BODY);
				}
				
				//blockDesc.flags = unit.flags;

				const PxReal maxImpulse0 = (unit.mFlags & PxcNpWorkUnitFlag::eARTICULATION_BODY0) ? static_cast<const PxsBodyCore*>(unit.mRigidCore0)->maxContactImpulse : data0.maxContactImpulse;
				const PxReal maxImpulse1 = (unit.mFlags & PxcNpWorkUnitFlag::eARTICULATION_BODY1) ? static_cast<const PxsBodyCore*>(unit.mRigidCore1)->maxContactImpulse : data1.maxContactImpulse;

				unit.setInvMassScaleFromDominance(blockDesc.invMassScales);

				blockDesc.restDistance = unit.mRestDistance;
				blockDesc.frictionPtr = unit.mFrictionDataPtr;
				blockDesc.frictionCount = unit.mFrictionPatchCount;
				blockDesc.maxCCDSeparation = PX_MAX_F32;
				blockDesc.maxImpulse = PxMin(maxImpulse0, maxImpulse1);
				blockDesc.torsionalPatchRadius = unit.mTorsionalPatchRadius;
				blockDesc.minTorsionalPatchRadius = unit.mMinTorsionalPatchRadius;
				blockDesc.offsetSlop = unit.mOffsetSlop;
			}

			SolverConstraintPrepState::Enum buildState = SolverConstraintPrepState::eUNBATCHABLE;

#if PX_USE_BLOCK_SOLVER
			if (hdr.stride == 4)
			{
				buildState = createFinalizeSolverContacts4Step(
					cmOutputs,
					threadContext,
					blockDescs,
					invStepDt,
					totalDt,
					invTotalDt,
					stepDt,
					mBounceThreshold,
					mFrictionOffsetThreshold,
					mCorrelationDistance,
					biasCoefficient,
					blockAllocator);
			}
#endif

			if (buildState != SolverConstraintPrepState::eSUCCESS)
			{
				for (PxU32 a = startIdx, i = 0; a < endIdx; ++a, i++)
				{
					PxSolverConstraintDesc& desc = contactDescPtr[a];
					PxsContactManager* cm = reinterpret_cast<PxsContactManager*>(desc.constraint);
					//PxcNpWorkUnit& n = cm->getWorkUnit();

					PxsContactManagerOutput& output = *cmOutputs[i];
					//PX_ASSERT(output.nbContacts != 0);

					createFinalizeSolverContactsStep(blockDescs[i], output, threadContext,
						invStepDt, invTotalDt, totalDt, stepDt, mBounceThreshold, mFrictionOffsetThreshold, mCorrelationDistance, biasCoefficient,
						blockAllocator);

					getContactManagerConstraintDesc(output, *cm, desc);

					//PX_ASSERT(desc.constraint != NULL);
				}
			}

			for (PxU32 a = startIdx, i = 0; a < endIdx; ++a, i++)
			{
				updateFrictionAnchorCountAndPosition(contactDescPtr[a], *cmOutputs[i], blockDescs[i]);
			}

			for (PxU32 i = 0; i < hdr.stride; ++i)
			{
				PxsContactManager* cm = cms[i];

				PxcNpWorkUnit& unit = cm->getWorkUnit();
				unit.mFrictionDataPtr = blockDescs[i].frictionPtr;
				unit.mFrictionPatchCount = blockDescs[i].frictionCount;
			}
		}
		else if (contactDescPtr[startIdx].constraintType == DY_SC_TYPE_RB_1D)
		{
			SolverConstraintShaderPrepDesc shaderPrepDescs[4];
			PxTGSSolverConstraintPrepDesc prepDescs[4];

			for (PxU32 a = startIdx, i = 0; a < endIdx; ++a, i++)
			{
				SolverConstraintShaderPrepDesc& shaderPrepDesc = shaderPrepDescs[i];
				PxTGSSolverConstraintPrepDesc& prepDesc = prepDescs[i];

				const PxTransform id(PxIdentity);

				{
					PxSolverConstraintDesc& desc = contactDescPtr[a];
					const Constraint* constraint = reinterpret_cast<const Constraint*>(desc.constraint);

					const PxConstraintSolverPrep solverPrep = constraint->solverPrep;
					const void* constantBlock = constraint->constantBlock;
					const PxU32 constantBlockByteSize = constraint->constantBlockSize;
					const PxTransform& pose0 = (constraint->body0 ? constraint->body0->getPose() : id);
					const PxTransform& pose1 = (constraint->body1 ? constraint->body1->getPose() : id);
					const PxTGSSolverBodyVel* sbody0 = desc.tgsBodyA;
					const PxTGSSolverBodyVel* sbody1 = desc.tgsBodyB;

					PxTGSSolverBodyTxInertia& txI0 = txInertias[desc.bodyADataIndex];
					PxTGSSolverBodyTxInertia& txI1 = txInertias[desc.bodyBDataIndex];

					PxTGSSolverBodyData& data0 = solverBodyDatas[desc.bodyADataIndex];
					PxTGSSolverBodyData& data1 = solverBodyDatas[desc.bodyBDataIndex];

					shaderPrepDesc.constantBlock = constantBlock;
					shaderPrepDesc.constantBlockByteSize = constantBlockByteSize;
					shaderPrepDesc.constraint = constraint;
					shaderPrepDesc.solverPrep = solverPrep;

					prepDesc.desc = &desc;
					prepDesc.bodyFrame0 = pose0;
					prepDesc.bodyFrame1 = pose1;
					prepDesc.body0 = sbody0;
					prepDesc.body1 = sbody1;
					prepDesc.body0TxI = &txI0;
					prepDesc.body1TxI = &txI1;
					prepDesc.bodyData0 = &data0;
					prepDesc.bodyData1 = &data1;
					prepDesc.linBreakForce = constraint->linBreakForce;
					prepDesc.angBreakForce = constraint->angBreakForce;
					prepDesc.writeback = &getConstraintWriteBackPool()[constraint->index];
					setupConstraintFlags(prepDesc, constraint->flags);
					prepDesc.minResponseThreshold = constraint->minResponseThreshold;

					prepDesc.bodyState0 = desc.linkIndexA == PxSolverConstraintDesc::RIGID_BODY ? PxSolverContactDesc::eDYNAMIC_BODY : PxSolverContactDesc::eARTICULATION;
					prepDesc.bodyState1 = desc.linkIndexB == PxSolverConstraintDesc::RIGID_BODY ? PxSolverContactDesc::eDYNAMIC_BODY : PxSolverContactDesc::eARTICULATION;
				}
			}

			SolverConstraintPrepState::Enum buildState = SolverConstraintPrepState::eUNBATCHABLE;

#if PX_USE_BLOCK_SOLVER
#if PX_USE_BLOCK_1D
			if (hdr.stride == 4)
			{
				PxU32 totalRows;
				buildState = setupSolverConstraintStep4
				(shaderPrepDescs, prepDescs, stepDt, totalDt, invStepDt, invTotalDt, totalRows, blockAllocator, mLengthScale, biasCoefficient, mIsResidualReportingEnabled);
			}
#endif
#endif
			if (buildState != SolverConstraintPrepState::eSUCCESS)
			{
				for (PxU32 a = startIdx, i = 0; a < endIdx; ++a, i++)
				{
					PxReal clampedInvDt = invStepDt;
					SetupSolverConstraintStep(shaderPrepDescs[i], prepDescs[i], blockAllocator, stepDt, totalDt, clampedInvDt, invTotalDt, mLengthScale,
						biasCoefficient);
				}
			}
		}
	}
}

void solveContactBlock		(DY_TGS_SOLVE_METHOD_PARAMS);
void solve1DBlock			(DY_TGS_SOLVE_METHOD_PARAMS);
void solveExtContactBlock	(DY_TGS_SOLVE_METHOD_PARAMS);
void solveExt1DBlock		(DY_TGS_SOLVE_METHOD_PARAMS);
void solveContact4			(DY_TGS_SOLVE_METHOD_PARAMS);
void solve1D4				(DY_TGS_SOLVE_METHOD_PARAMS);

TGSSolveBlockMethod g_SolveTGSMethods[] = 
{
	0,
	solveContactBlock,		// DY_SC_TYPE_RB_CONTACT
	solve1DBlock,			// DY_SC_TYPE_RB_1D
	solveExtContactBlock,	// DY_SC_TYPE_EXT_CONTACT
	solveExt1DBlock,		// DY_SC_TYPE_EXT_1D
	solveContactBlock,		// DY_SC_TYPE_STATIC_CONTACT
	solveContact4,			// DY_SC_TYPE_BLOCK_RB_CONTACT
	solveContact4,			// DY_SC_TYPE_BLOCK_STATIC_RB_CONTACT
	solve1D4,				// DY_SC_TYPE_BLOCK_1D,
};

void writeBackContact	(DY_TGS_WRITEBACK_METHOD_PARAMS);
void writeBack1D		(DY_TGS_WRITEBACK_METHOD_PARAMS);
void writeBackContact4	(DY_TGS_WRITEBACK_METHOD_PARAMS);
void writeBack1D4		(DY_TGS_WRITEBACK_METHOD_PARAMS);

TGSWriteBackMethod g_WritebackTGSMethods[] =
{
	0,
	writeBackContact,		// DY_SC_TYPE_RB_CONTACT
	writeBack1D,			// DY_SC_TYPE_RB_1D
	writeBackContact,		// DY_SC_TYPE_EXT_CONTACT
	writeBack1D,			// DY_SC_TYPE_EXT_1D
	writeBackContact,		// DY_SC_TYPE_STATIC_CONTACT
	writeBackContact4,		// DY_SC_TYPE_BLOCK_RB_CONTACT
	writeBackContact4,		// DY_SC_TYPE_BLOCK_STATIC_RB_CONTACT
	writeBack1D4,			// DY_SC_TYPE_BLOCK_1D,
};

void solveConclude1DBlock			(DY_TGS_CONCLUDE_METHOD_PARAMS);
void solveConcludeContactBlock		(DY_TGS_CONCLUDE_METHOD_PARAMS);
void solveConcludeContact4			(DY_TGS_CONCLUDE_METHOD_PARAMS);
void solveConclude1D4				(DY_TGS_CONCLUDE_METHOD_PARAMS);
void solveConcludeContactExtBlock	(DY_TGS_CONCLUDE_METHOD_PARAMS);
void solveConclude1DBlockExt		(DY_TGS_CONCLUDE_METHOD_PARAMS);

TGSSolveConcludeMethod g_SolveConcludeTGSMethods[] =
{
	0,
	solveConcludeContactBlock,		// DY_SC_TYPE_RB_CONTACT
	solveConclude1DBlock,			// DY_SC_TYPE_RB_1D
	solveConcludeContactExtBlock,	// DY_SC_TYPE_EXT_CONTACT
	solveConclude1DBlockExt,		// DY_SC_TYPE_EXT_1D
	solveConcludeContactBlock,		// DY_SC_TYPE_STATIC_CONTACT
	solveConcludeContact4,			// DY_SC_TYPE_BLOCK_RB_CONTACT
	solveConcludeContact4,			// DY_SC_TYPE_BLOCK_STATIC_RB_CONTACT
	solveConclude1D4,				// DY_SC_TYPE_BLOCK_1D,
};

void DynamicsTGSContext::solveConstraintsIteration(const PxSolverConstraintDesc* const contactDescPtr, const PxConstraintBatchHeader* const batchHeaders, PxU32 nbHeaders,
	PxReal invStepDt, const PxTGSSolverBodyTxInertia* const solverTxInertia, PxReal elapsedTime, PxReal minPenetration, SolverContext& cache)
{
	PX_UNUSED(invStepDt);

	for (PxU32 h = 0; h < nbHeaders; ++h)
	{
		const PxConstraintBatchHeader& hdr = batchHeaders[h];
		g_SolveTGSMethods[hdr.constraintType](hdr, contactDescPtr, solverTxInertia, minPenetration, elapsedTime, cache);
	}
}

template <bool TSync>
void DynamicsTGSContext::parallelSolveConstraints(const PxSolverConstraintDesc* const contactDescPtr, const PxConstraintBatchHeader* const batchHeaders, PxU32 nbHeaders,
	PxTGSSolverBodyTxInertia* solverTxInertia, PxReal elapsedTime, PxReal minPenetration,
	SolverContext& cache, PxU32 iterCount)
{
	for (PxU32 h = 0; h < nbHeaders; ++h)
	{
		const PxConstraintBatchHeader& hdr = batchHeaders[h];

		const PxSolverConstraintDesc& desc = contactDescPtr[hdr.startIndex];

		if (TSync)
		{
			PX_ASSERT(hdr.stride == 1);
			waitForProgresses(desc, iterCount);
		}

		g_SolveTGSMethods[hdr.constraintType](hdr, contactDescPtr, solverTxInertia, minPenetration, elapsedTime, cache);

		if (TSync)
		{
			PxMemoryBarrier();
			incrementProgress(desc);
		}
	}
}

void DynamicsTGSContext::writebackConstraintsIteration(const PxConstraintBatchHeader* const hdrs, const PxSolverConstraintDesc* const contactDescPtr, PxU32 nbHeaders, SolverContext& cache)
{
	PX_PROFILE_ZONE("Writeback", mContextID);

	for (PxU32 h = 0; h < nbHeaders; ++h)
	{
		const PxConstraintBatchHeader& hdr = hdrs[h];

		g_WritebackTGSMethods[hdr.constraintType](hdr, contactDescPtr, &cache);	
	}
}

void DynamicsTGSContext::parallelWritebackConstraintsIteration(const PxSolverConstraintDesc* const contactDescPtr, const PxConstraintBatchHeader* const batchHeaders, PxU32 nbHeaders, SolverContext& cache)
{
	for (PxU32 h = 0; h < nbHeaders; ++h)
	{
		const PxConstraintBatchHeader& hdr = batchHeaders[h];

		g_WritebackTGSMethods[hdr.constraintType](hdr, contactDescPtr, &cache);
	}
}

template <bool TSync>
void DynamicsTGSContext::solveConcludeConstraintsIteration(const PxSolverConstraintDesc* const contactDescPtr,
	const PxConstraintBatchHeader* const batchHeaders, PxU32 nbHeaders, PxTGSSolverBodyTxInertia* solverTxInertia, 
	PxReal elapsedTime, SolverContext& cache, PxU32 iterCount)
{
	for (PxU32 h = 0; h < nbHeaders; ++h)
	{
		const PxConstraintBatchHeader& hdr = batchHeaders[h];

		const PxSolverConstraintDesc& desc = contactDescPtr[hdr.startIndex];
		if (TSync)
			waitForProgresses(desc, iterCount);

		g_SolveConcludeTGSMethods[hdr.constraintType](hdr, contactDescPtr, solverTxInertia, elapsedTime, cache);

		if (TSync)
		{
			PxMemoryBarrier();
			incrementProgress(desc);
		}
	}
}

void integrateCoreStep(PxTGSSolverBodyVel& vel, PxTGSSolverBodyTxInertia& txInertia, PxF32 dt)
{
	PxU32 lockFlags = vel.lockFlags;
	if (lockFlags)
	{
		if (lockFlags & PxRigidDynamicLockFlag::eLOCK_LINEAR_X)
			vel.linearVelocity.x = 0.f;
		if (lockFlags & PxRigidDynamicLockFlag::eLOCK_LINEAR_Y)
			vel.linearVelocity.y = 0.f;
		if (lockFlags & PxRigidDynamicLockFlag::eLOCK_LINEAR_Z)
			vel.linearVelocity.z = 0.f;

		//The angular velocity should be 0 because it is now impossible to make it rotate around that axis!
		if (lockFlags & PxRigidDynamicLockFlag::eLOCK_ANGULAR_X)
			vel.angularVelocity.x = 0.f;
		if (lockFlags & PxRigidDynamicLockFlag::eLOCK_ANGULAR_Y)
			vel.angularVelocity.y = 0.f;
		if (lockFlags & PxRigidDynamicLockFlag::eLOCK_ANGULAR_Z)
			vel.angularVelocity.z = 0.f;
	}

	PxVec3 linearMotionVel = vel.linearVelocity;
	const PxVec3 delta = linearMotionVel * dt;

	//The solver accumulates deltaAngularMomocity.
	//We need to translate from deltaAngularMomocity to deltaAngularVelocity.
	//deltaAngularVelocity = I^(-1/2)*deltaAngularMomocity
	PxVec3 unmolestedAngVel = vel.angularVelocity;
	PxVec3 angularMotionVel = txInertia.sqrtInvInertia * vel.angularVelocity;
	PxReal w2 = angularMotionVel.magnitudeSquared();
	txInertia.body2WorldP += delta;
	PX_ASSERT(txInertia.body2WorldP.isFinite());

	// Integrate the rotation using closed form quaternion integrator
	if (w2 != 0.0f)
	{
		PxReal w = PxSqrt(w2);
		
		//KS - we allow a little bit more angular velocity than the default
		//maxAngVel member otherwise the simulation feels a little flimsy
		/*const PxReal maxW = PxMax(50.f, vel.maxAngVel);

		if (w > maxW)
		{
			PxReal ratio = maxW / w;

			vel.angularVelocity *= ratio;
		}*/

		const PxReal v = dt * w * 0.5f;
		PxReal s, q;
		PxSinCos(v, s, q);
		s /= w;

		const PxVec3 pqr = angularMotionVel * s;
		const PxQuat quatVel(pqr.x, pqr.y, pqr.z, 0);
		PxQuat result = quatVel * txInertia.deltaBody2WorldQ;

		result += txInertia.deltaBody2WorldQ * q;

		txInertia.deltaBody2WorldQ = result.getNormalized();
		PX_ASSERT(txInertia.deltaBody2WorldQ.isSane());
		PX_ASSERT(txInertia.deltaBody2WorldQ.isFinite());

		//Accumulate the angular rotations in a space we can project the angular constraints to	
	}

	vel.deltaAngDt += unmolestedAngVel * dt;
	vel.deltaLinDt += delta;

	/*solverBodyData.body2World = txInertia.body2World;
	solverBodyData.deltaLinDt = vel.deltaLinDt;
	solverBodyData.deltaAngDt = vel.deltaAngDt;*/
}

void DynamicsTGSContext::applySubstepGravity(PxsRigidBody** bodies, PxsExternalAccelerationProvider& externalAccelerations, 
	PxU32 count, PxTGSSolverBodyVel* vels, PxReal dt, PxTGSSolverBodyTxInertia* PX_RESTRICT txInertias, PxU32* nodeIndexArray)
{
	const PxVec3 gravityTimesDt = dt * mGravity;
	const bool hasExternalAcceleration = externalAccelerations.hasAccelerations();
	for (PxU32 k = 0; k < count; k++)
	{
		const PxsRigidBody& rBody = *bodies[k];
		const PxsBodyCore& core = rBody.getCore();
		
		const PxsRigidBodyExternalAcceleration* acc = hasExternalAcceleration ? &externalAccelerations.get(nodeIndexArray[k]) : NULL;

		const PxU16 lockFlags = core.lockFlags;
		if (acc || !core.disableGravity)
		{
			const PxVec3 gravityContribution = core.disableGravity ? PxVec3(0.0f) : (gravityTimesDt * rBody.mAccelScale);
			const PxVec3 linearAcceleration = acc ? (acc->linearAcceleration * dt) : PxVec3(0.0f);

			PxVec3 v = vels[k + 1].linearVelocity + gravityContribution + linearAcceleration;

			v.x = lockFlags & PxRigidDynamicLockFlag::eLOCK_LINEAR_X ? 0.f : v.x;
			v.y = lockFlags & PxRigidDynamicLockFlag::eLOCK_LINEAR_Y ? 0.f : v.y;
			v.z = lockFlags & PxRigidDynamicLockFlag::eLOCK_LINEAR_Z ? 0.f : v.z;			

			vels[k + 1].linearVelocity = v;
		}

		if (acc && acc->angularAcceleration != PxVec3(0.0f))
		{
			PxVec3 a = vels[k + 1].angularVelocity + txInertias[k + 1].sqrtInvInertia.getInverse() * acc->angularAcceleration * dt;

			a.x = lockFlags & PxRigidDynamicLockFlag::eLOCK_ANGULAR_X ? 0.f : a.x;
			a.y = lockFlags & PxRigidDynamicLockFlag::eLOCK_ANGULAR_Y ? 0.f : a.y;
			a.z = lockFlags & PxRigidDynamicLockFlag::eLOCK_ANGULAR_Z ? 0.f : a.z;

			vels[k + 1].angularVelocity = a;
		}
	}
}

void DynamicsTGSContext::integrateBodies(PxU32 count, PxTGSSolverBodyVel* PX_RESTRICT vels,
	PxTGSSolverBodyTxInertia* PX_RESTRICT txInertias, PxReal dt)
{
	for (PxU32 k = 0; k < count; k++)
	{
		integrateCoreStep(vels[k + 1], txInertias[k + 1], dt);
	}
}

void DynamicsTGSContext::integrateBodiesAndApplyGravity(const SolverIslandObjectsStep& objects,
	PxU32 count, PxTGSSolverBodyVel* PX_RESTRICT vels, PxTGSSolverBodyTxInertia* PX_RESTRICT txInertias,
	PxReal dt, PxU32 posIters)
{
	for (PxU32 i = 0; i < posIters; i++)
	{
		applySubstepGravity(objects.bodies, *objects.externalAccelerations, count, vels, dt, txInertias, objects.nodeIndexArray);
		integrateBodies(count, vels, txInertias, dt);
	}
}

void DynamicsTGSContext::parallelIntegrateBodies(PxTGSSolverBodyVel* vels, PxTGSSolverBodyTxInertia* txInertias,
	PxU32 count, PxReal dt, PxU32 iteration)
{
	for (PxU32 k = 0; k < count; k++)
	{
		PX_ASSERT(vels[k + 1].partitionMask == (iteration * vels[k + 1].nbStaticInteractions));
		PX_UNUSED(iteration);
		integrateCoreStep(vels[k + 1], txInertias[k + 1], dt);
	}
}

void DynamicsTGSContext::copyBackBodies(const SolverIslandObjectsStep& objects,
	PxTGSSolverBodyVel* vels, PxTGSSolverBodyTxInertia* txInertias,
	PxTGSSolverBodyData* solverBodyDatas, PxReal invDt, IG::IslandSim& islandSim,
	PxU32 startIdx, PxU32 endIdx)
{
	for (PxU32 k = startIdx; k < endIdx; k++)
	{
		//PxStepSolverBody& solverBodyData = solverBodyData2[k + 1];
		PxTGSSolverBodyVel& solverBodyVel = vels[k + 1];
		PxTGSSolverBodyTxInertia& solverBodyTxI = txInertias[k + 1];
		PxTGSSolverBodyData& solverBodyData = solverBodyDatas[k + 1];

		const Cm::SpatialVector motionVel(solverBodyVel.deltaLinDt*invDt, solverBodyTxI.sqrtInvInertia*(solverBodyVel.deltaAngDt*invDt));

		PxsRigidBody& rBody = *objects.bodies[k];
		PxsBodyCore& core = rBody.getCore();
		rBody.mLastTransform = core.body2World;
		core.body2World.q = (solverBodyTxI.deltaBody2WorldQ * core.body2World.q).getNormalized();
		core.body2World.p = solverBodyTxI.body2WorldP;
		/*core.linearVelocity = (solverBodyVel.linearVelocity);
		core.angularVelocity = solverBodyTxI.sqrtInvInertia*(solverBodyVel.angularVelocity);*/

		PxVec3 linearVelocity = (solverBodyVel.linearVelocity);
		PxVec3 angularVelocity = solverBodyTxI.sqrtInvInertia*(solverBodyVel.angularVelocity);

		core.linearVelocity = linearVelocity;
		core.angularVelocity = angularVelocity;

		const PxU32 hasStaticTouch = islandSim.getIslandStaticTouchCount(PxNodeIndex(solverBodyData.nodeIndex));
		sleepCheck(&rBody, mDt, mEnableStabilization, motionVel, hasStaticTouch);
	}
}

void DynamicsTGSContext::stepArticulations(Dy::ThreadContext& threadContext, const PxsIslandIndices& counts, PxReal dt, PxReal totalInvDt)
{
	for (PxU32 a = 0; a < counts.articulations; ++a)
	{
		ArticulationSolverDesc& d = threadContext.getArticulations()[a];
		//if(d.articulation->numTotalConstraints > 0)
		//d.articulation->solveInternalConstraints(dt, 1.f / dt, threadContext.mZVector.begin(), threadContext.mDeltaV.begin(), false);
		ArticulationPImpl::updateDeltaMotion(d, dt, threadContext.mDeltaV.begin(), totalInvDt);		
	}
}

void DynamicsTGSContext::updateArticulations(Dy::ThreadContext& threadContext, PxU32 startIdx, PxU32 endIdx, PxReal dt)
{
	for (PxU32 a = startIdx; a < endIdx; ++a)
	{
		ArticulationSolverDesc& d = threadContext.getArticulations()[a];
		ArticulationPImpl::updateBodiesTGS(d, threadContext.mDeltaV.begin(), dt);
	}
}

void DynamicsTGSContext::applyArticulationTgsSubstepForces(Dy::ThreadContext& threadContext, PxU32 numArticulations, PxReal stepDt)
{
	for(PxU32 i = 0; i < numArticulations; ++i)
	{
		ArticulationSolverDesc& d = threadContext.getArticulations()[i];

		// zVector just used as temp buffer
		threadContext.mZVector.resizeUninitialized(d.linkCount);
		FeatherstoneArticulation::applyTgsSubstepForces(d, stepDt, threadContext.mZVector.begin());
	}

}

namespace
{
class ArticulationTask : public Cm::Task
{
	Dy::DynamicsTGSContext& mContext;
	const ArticulationSolverDesc* const mDescs;
	const PxU32 mNbDescs;
	const PxVec3 mGravity;
	const PxReal mDt;
	const bool mExternalForcesEveryTgsIterationEnabled;

	PX_NOCOPY(ArticulationTask)

public:
	static const PxU32 MaxNbPerTask = 32;

	ArticulationTask(Dy::DynamicsTGSContext& context, const ArticulationSolverDesc* descs, PxU32 nbDescs,
	                 const PxVec3& gravity, PxReal dt, PxU64 contextId, bool externalForcesEveryTgsIterationEnabled)
	: Cm::Task(contextId)
	, mContext(context)
	, mDescs(descs)
	, mNbDescs(nbDescs)
	, mGravity(gravity)
	, mDt(dt)
	, mExternalForcesEveryTgsIterationEnabled(externalForcesEveryTgsIterationEnabled)
	{
	}

	virtual const char* getName() const { return "ArticulationTask";  }

	virtual void runInternal()
	{
		PxU32 maxLinks = 0;
		for (PxU32 i = 0; i < mNbDescs; i++)
		{
			maxLinks = PxMax(maxLinks, PxU32(mDescs[i].linkCount));
		}

		Dy::ThreadContext& threadContext = *mContext.getThreadContext();

		threadContext.mDeltaV.forceSize_Unsafe(0);
		threadContext.mDeltaV.reserve(maxLinks);
		threadContext.mDeltaV.forceSize_Unsafe(maxLinks);

		const PxReal invLengthScale = 1.f / mContext.getLengthScale();

		for (PxU32 a = 0; a < mNbDescs; ++a)
		{			
			ArticulationPImpl::computeUnconstrainedVelocitiesTGS(mDescs[a], mDt, 
				mGravity, invLengthScale, mExternalForcesEveryTgsIterationEnabled);
		}

		mContext.putThreadContext(&threadContext);
	}
};
}

void DynamicsTGSContext::setupArticulations(IslandContextStep& islandContext, const PxVec3& gravity, PxReal dt, PxU32& posIters,
	PxU32& velIters, PxBaseTask* continuation)
{
	Dy::FeatherstoneArticulation** articulations = islandContext.mThreadContext->mArticulationArray;
	PxU32 nbArticulations = islandContext.mCounts.articulations;

	PxU32 maxVelIters = 0;
	PxU32 maxPosIters = 0;

	for (PxU32 a = 0; a < nbArticulations; a+= ArticulationTask::MaxNbPerTask)
	{
		const PxU32 endIdx = PxMin(nbArticulations, a + ArticulationTask::MaxNbPerTask);
		for (PxU32 b = a; b < endIdx; ++b)
		{
			ArticulationSolverDesc& desc = islandContext.mThreadContext->getArticulations()[b];
			articulations[b]->getSolverDesc(desc);
			articulations[b]->mArticulationIndex = PxU16(b);

			const PxU16 iterWord = articulations[b]->getIterationCounts();
			maxVelIters = PxMax<PxU32>(PxU32(iterWord >> 8), maxVelIters);
			maxPosIters = PxMax<PxU32>(PxU32(iterWord & 0xff), maxPosIters);
		}

		Dy::ThreadContext* threadContext = islandContext.mThreadContext;
		ArticulationTask* task = PX_PLACEMENT_NEW(mTaskPool.allocate(sizeof(ArticulationTask)), ArticulationTask)(*this, threadContext->getArticulations().begin() + a,
			endIdx - a, gravity, dt, getContextId(), mIsExternalForcesEveryTgsIterationEnabled);

		task->setContinuation(continuation);
		task->removeReference();
	}

	velIters = PxMax(maxVelIters, velIters);
	posIters = PxMax(maxPosIters, posIters);
}

PxU32 DynamicsTGSContext::setupArticulationInternalConstraints(IslandContextStep& islandContext, PxReal dt, PxReal invStepDt)
{
	Dy::FeatherstoneArticulation** articulations = islandContext.mThreadContext->mArticulationArray;
	PxU32 nbArticulations = islandContext.mCounts.articulations;

	ThreadContext* threadContext = getThreadContext();
	threadContext->mConstraintBlockStream.reset();

	PxU32 totalDescCount = 0;

	for (PxU32 a = 0; a < nbArticulations; ++a)
	{
		ArticulationSolverDesc& desc = islandContext.mThreadContext->getArticulations()[a];
		articulations[a]->getSolverDesc(desc);

		PxU32 descCount = ArticulationPImpl::setupSolverInternalConstraintsTGS(desc, 
			islandContext.mStepDt, invStepDt, dt);

		desc.numInternalConstraints = PxTo8(descCount);

		totalDescCount += descCount;
	}

	putThreadContext(threadContext);

	islandContext.mThreadContext->contactDescArraySize += totalDescCount;

	return totalDescCount;
}

class SetupDescsTask : public Cm::Task
{
	Dy::IslandContextStep& mIslandContext;
	const SolverIslandObjectsStep& mObjects;
	PxU32* mBodyRemapTable; 
	PxU32 mSolverBodyOffset; 
	PxsContactManagerOutputIterator& mOutputs;
	DynamicsTGSContext& mContext;

	PX_NOCOPY(SetupDescsTask)

public:

	SetupDescsTask(IslandContextStep& islandContext, const SolverIslandObjectsStep& objects,
		PxU32* bodyRemapTable, PxU32 solverBodyOffset, PxsContactManagerOutputIterator& outputs, DynamicsTGSContext& context) : Cm::Task(context.getContextId()),
		mIslandContext(islandContext), mObjects(objects), mBodyRemapTable(bodyRemapTable), mSolverBodyOffset(solverBodyOffset),
		mOutputs(outputs), mContext(context)
	{
	}

	virtual const char* getName() const { return "SetupDescsTask"; }

	virtual void runInternal()
	{
		mContext.setupDescs(mIslandContext, mObjects, mBodyRemapTable, mSolverBodyOffset, mOutputs);
		mIslandContext.mArticulationOffset = mIslandContext.mThreadContext->contactDescArraySize;
	}
};

class PreIntegrateParallelTask : public Cm::Task
{
	PxsBodyCore** mBodyArray;
	PxsRigidBody** mOriginalBodyArray;
	PxTGSSolverBodyVel* mSolverBodyVelPool;
	PxTGSSolverBodyTxInertia* mSolverBodyTxInertia;
	PxTGSSolverBodyData* mSolverBodyDataPool2;
	PxU32* mNodeIndexArray;
	const PxU32 mBodyCount;
	const PxVec3& mGravity;
	const PxReal mDt;
	PxU32& mPosIters;
	PxU32& mVelIters;
	DynamicsTGSContext& mContext;

	PX_NOCOPY(PreIntegrateParallelTask)

public:

	PreIntegrateParallelTask(PxsBodyCore** bodyArray, PxsRigidBody** originalBodyArray,
		PxTGSSolverBodyVel* solverBodyVelPool, PxTGSSolverBodyTxInertia* solverBodyTxInertia, PxTGSSolverBodyData* solverBodyDataPool2,
		PxU32* nodeIndexArray, PxU32 bodyCount, const PxVec3& gravity, PxReal dt, PxU32& posIters, PxU32& velIters,
		DynamicsTGSContext& context) : Cm::Task(context.getContextId()),
		mBodyArray(bodyArray), mOriginalBodyArray(originalBodyArray), mSolverBodyVelPool(solverBodyVelPool),
		mSolverBodyTxInertia(solverBodyTxInertia), mSolverBodyDataPool2(solverBodyDataPool2), mNodeIndexArray(nodeIndexArray),
		mBodyCount(bodyCount), mGravity(gravity), mDt(dt), mPosIters(posIters), mVelIters(velIters), mContext(context)
	{
	}

	virtual const char* getName() const { return "PreIntegrateParallelTask"; }

	virtual void runInternal()
	{
		PxU32 posIters = 0;
		PxU32 velIters = 0;
		mContext.preIntegrateBodies(mBodyArray, mOriginalBodyArray, mSolverBodyVelPool, mSolverBodyTxInertia, mSolverBodyDataPool2, mNodeIndexArray, mBodyCount, mGravity, mDt, posIters, velIters, 0);

		PxAtomicMax(reinterpret_cast<PxI32*>(&mPosIters), PxI32(posIters));
		PxAtomicMax(reinterpret_cast<PxI32*>(&mVelIters), PxI32(velIters));
	}
};

class PreIntegrateTask : public Cm::Task
{
	PxsBodyCore** mBodyArray;
	PxsRigidBody** mOriginalBodyArray;
	PxTGSSolverBodyVel* mSolverBodyVelPool;
	PxTGSSolverBodyTxInertia* mSolverBodyTxInertia;
	PxTGSSolverBodyData* mSolverBodyDataPool2;
	PxU32* mNodeIndexArray;
	const PxU32 mBodyCount; 
	const PxVec3& mGravity;
	const PxReal mDt;
	PxU32& mPosIters;
	PxU32& mVelIters; 
	DynamicsTGSContext& mContext;

	PX_NOCOPY(PreIntegrateTask)

public:

	PreIntegrateTask(PxsBodyCore** bodyArray, PxsRigidBody** originalBodyArray,
		PxTGSSolverBodyVel* solverBodyVelPool, PxTGSSolverBodyTxInertia* solverBodyTxInertia, PxTGSSolverBodyData* solverBodyDataPool2,
		PxU32* nodeIndexArray, PxU32 bodyCount, const PxVec3& gravity, PxReal dt, PxU32& posIters, PxU32& velIters,
		DynamicsTGSContext& context) : Cm::Task(context.getContextId()),
		mBodyArray(bodyArray), mOriginalBodyArray(originalBodyArray), mSolverBodyVelPool(solverBodyVelPool),
		mSolverBodyTxInertia(solverBodyTxInertia), mSolverBodyDataPool2(solverBodyDataPool2), mNodeIndexArray(nodeIndexArray), 
		mBodyCount(bodyCount), mGravity(gravity), mDt(dt), mPosIters(posIters), mVelIters(velIters), mContext(context)
	{
	}

	virtual const char* getName() const { return "PreIntegrateTask"; }

	virtual void runInternal()
	{
		const PxU32 BodiesPerTask = 512;

		if (mBodyCount <= BodiesPerTask)
		{
			PxU32 posIters = 0;
			PxU32 velIters = 0;
			mContext.preIntegrateBodies(mBodyArray, mOriginalBodyArray, mSolverBodyVelPool, mSolverBodyTxInertia, mSolverBodyDataPool2, mNodeIndexArray, mBodyCount, mGravity, mDt, posIters, velIters, 0);

			PxAtomicMax(reinterpret_cast<PxI32*>(&mPosIters), PxI32(posIters));
			PxAtomicMax(reinterpret_cast<PxI32*>(&mVelIters), PxI32(velIters));
		}
		else
		{
			for (PxU32 i = 0; i < mBodyCount; i += BodiesPerTask)
			{
				const PxU32 nbToProcess = PxMin(mBodyCount - i, BodiesPerTask);
				PreIntegrateParallelTask* task = PX_PLACEMENT_NEW(mContext.getTaskPool().allocate(sizeof(PreIntegrateParallelTask)), PreIntegrateParallelTask)
					(mBodyArray+i, mOriginalBodyArray+i, mSolverBodyVelPool+i, mSolverBodyTxInertia+i, mSolverBodyDataPool2+i,mNodeIndexArray+i,
						nbToProcess, mGravity, mDt, mPosIters, mVelIters, mContext);

				task->setContinuation(mCont);
				task->removeReference();
			}
		}
	}
};

class SetStepperTask : public Cm::Task
{
	Dy::IslandContextStep& mIslandContext;
	Dy::DynamicsTGSContext& mContext;

	PxBaseTask* mAdditionalContinuation;

	PX_NOCOPY(SetStepperTask)

public:

	SetStepperTask(Dy::IslandContextStep& islandContext,
		DynamicsTGSContext& context) : Cm::Task(context.getContextId()),
		mIslandContext(islandContext), mContext(context),
		mAdditionalContinuation(NULL)
	{
	}

	virtual const char* getName() const { return "SetStepperTask"; }

	void setAdditionalContinuation(PxBaseTask* cont)
	{
		mAdditionalContinuation = cont;
		cont->addReference();
	}

	virtual void runInternal()
	{
		PxReal dt = mContext.getDt();

		mIslandContext.mStepDt = dt / PxReal(mIslandContext.mPosIters);
		mIslandContext.mInvStepDt = 1.f/mIslandContext.mStepDt;//PxMin(1000.f, 1.f / mIslandContext.mStepDt);
		mIslandContext.mBiasCoefficient = 2.f * PxSqrt(1.f/mIslandContext.mPosIters);
	}

	virtual void release()
	{
		Cm::Task::release();
		mAdditionalContinuation->removeReference();
	}
};

class SetupArticulationTask : public Cm::Task
{
	IslandContextStep& mIslandContext;
	const PxVec3& mGravity;
	const PxReal mDt;
	PxU32& mPosIters;
	PxU32& mVelIters;

	DynamicsTGSContext& mContext;

	PX_NOCOPY(SetupArticulationTask)

public:

	SetupArticulationTask(IslandContextStep& islandContext, const PxVec3& gravity, PxReal dt, PxU32& posIters,
		PxU32& velIters, DynamicsTGSContext& context) : Cm::Task(context.getContextId()),
		mIslandContext(islandContext), mGravity(gravity), mDt(dt), mPosIters(posIters), mVelIters(velIters), mContext(context)
	{
	}

	virtual const char* getName() const { return "SetupArticulationTask"; }

	virtual void runInternal()
	{
		PxU32 posIters = 0, velIters = 0;
		mContext.setupArticulations(mIslandContext, mGravity, mDt, posIters, velIters, mCont);

		PxAtomicMax(reinterpret_cast<PxI32*>(&mPosIters), PxI32(posIters));
		PxAtomicMax(reinterpret_cast<PxI32*>(&mVelIters), PxI32(velIters));
	}
};

class SetupArticulationInternalConstraintsTask : public Cm::Task
{
	IslandContextStep& mIslandContext;
	const PxReal mDt;
	const PxReal mInvDt;

	DynamicsTGSContext& mContext;

	PX_NOCOPY(SetupArticulationInternalConstraintsTask)

public:

	SetupArticulationInternalConstraintsTask(IslandContextStep& islandContext, PxReal dt, PxReal invDt, DynamicsTGSContext& context) : Cm::Task(context.getContextId()),
		mIslandContext(islandContext), mDt(dt), mInvDt(invDt), mContext(context)
	{
	}

	virtual const char* getName() const { return "SetupArticulationInternalConstraintsTask"; }

	virtual void runInternal()
	{
		mContext.setupArticulationInternalConstraints(mIslandContext, mDt, mIslandContext.mInvStepDt);
	}
};

class SetupSolverConstraintsSubTask : public Cm::Task
{
	PxSolverConstraintDesc* mContactDescPtr;
	PxConstraintBatchHeader* mHeaders;
	const PxU32 mNbHeaders;
	PxsContactManagerOutputIterator& mOutputs;
	PxReal mStepDt;
	PxReal mTotalDt;
	PxReal mInvStepDt; 
	PxReal mInvDtTotal;
	PxReal mBiasCoefficient;
	DynamicsTGSContext& mContext;
	ThreadContext& mIslandThreadContext;

	PX_NOCOPY(SetupSolverConstraintsSubTask)

public:

	static const PxU32 MaxPerTask = 64;

	SetupSolverConstraintsSubTask(PxSolverConstraintDesc* contactDescPtr, PxConstraintBatchHeader* headers, PxU32 nbHeaders,
		PxsContactManagerOutputIterator& outputs, PxReal stepDt, PxReal totalDt, PxReal invStepDt, PxReal invDtTotal, 
		PxReal biasCoefficient, ThreadContext& islandThreadContext, DynamicsTGSContext& context) : Cm::Task(context.getContextId()),
		mContactDescPtr(contactDescPtr), mHeaders(headers), mNbHeaders(nbHeaders), mOutputs(outputs), mStepDt(stepDt), mTotalDt(totalDt), mInvStepDt(invStepDt),
		mInvDtTotal(invDtTotal), mBiasCoefficient(biasCoefficient), mContext(context), mIslandThreadContext(islandThreadContext)
	{
	}

	virtual const char* getName() const { return "SetupSolverConstraintsSubTask"; }

	virtual void runInternal()
	{
		ThreadContext* tempContext = mContext.getThreadContext();
		tempContext->mConstraintBlockStream.reset();
		mContext.createSolverConstraints(mContactDescPtr, mHeaders, mNbHeaders, mOutputs, mIslandThreadContext, *tempContext, mStepDt, mTotalDt, mInvStepDt,
			mBiasCoefficient);
		mContext.putThreadContext(tempContext);
	}
};

class PxsCreateArticConstraintsSubTask : public Cm::Task
{
	PxsCreateArticConstraintsSubTask& operator=(const PxsCreateArticConstraintsSubTask&);

public:

	static const PxU32 NbArticsPerTask = 64;

	PxsCreateArticConstraintsSubTask(Dy::FeatherstoneArticulation** articulations, const PxU32 nbArticulations,
		PxTGSSolverBodyData* solverBodyData, PxTGSSolverBodyTxInertia* solverBodyTxInertia, 
		ThreadContext& threadContext, DynamicsTGSContext& context, PxsContactManagerOutputIterator& outputs,
		Dy::IslandContextStep& islandContext) :
		Cm::Task(context.getContextId()),
		mArticulations(articulations),
		mNbArticulations(nbArticulations),
		mSolverBodyData(solverBodyData),
		mSolverBodyTxInertia(solverBodyTxInertia),
		mThreadContext(threadContext), mDynamicsContext(context),
		mOutputs(outputs),
		mIslandContext(islandContext)
	{}

	virtual void runInternal()
	{
		const PxReal correlationDist = mDynamicsContext.getCorrelationDistance();
		const PxReal bounceThreshold = mDynamicsContext.getBounceThreshold();
		const PxReal frictionOffsetThreshold = mDynamicsContext.getFrictionOffsetThreshold();
		const PxReal dt = mDynamicsContext.getDt();
		
		const PxReal invStepDt = PxMin(mDynamicsContext.getMaxBiasCoefficient(), mIslandContext.mInvStepDt);
		const PxReal invDt = 1.0f / dt;

		ThreadContext* threadContext = mDynamicsContext.getThreadContext();
		threadContext->mConstraintBlockStream.reset(); //ensure there's no left-over memory that belonged to another island

		/*threadContext->mZVector.forceSize_Unsafe(0);
		threadContext->mZVector.reserve(mThreadContext.mMaxArticulationLinks);
		threadContext->mZVector.forceSize_Unsafe(mThreadContext.mMaxArticulationLinks);*/

		for (PxU32 i = 0; i < mNbArticulations; ++i)
		{
			mArticulations[i]->prepareStaticConstraintsTGS(mIslandContext.mStepDt, dt, invStepDt, invDt, mOutputs, *threadContext, correlationDist, bounceThreshold, frictionOffsetThreshold,
				mSolverBodyData, mSolverBodyTxInertia, mThreadContext.mConstraintBlockManager, mDynamicsContext.getConstraintWriteBackPool().begin(),
				mIslandContext.mBiasCoefficient, mDynamicsContext.getLengthScale());
		}

		mDynamicsContext.putThreadContext(threadContext);
	}

	virtual const char* getName() const
	{
		return "PxsDynamics.PxsCreateArticConstraintsSubTask";
	}

public:

	Dy::FeatherstoneArticulation** mArticulations;
	PxU32 mNbArticulations;
	PxTGSSolverBodyData* mSolverBodyData;
	PxTGSSolverBodyTxInertia* mSolverBodyTxInertia;
	ThreadContext& mThreadContext;
	DynamicsTGSContext& mDynamicsContext;
	PxsContactManagerOutputIterator& mOutputs;
	IslandContextStep& mIslandContext;
};

class SetupSolverConstraintsTask : public Cm::Task
{
	IslandContextStep& mIslandContext;
	PxSolverConstraintDesc* mContactDescPtr;
	PxsContactManagerOutputIterator& mOutputs;
	Dy::ThreadContext& mThreadContext;
	PxReal mTotalDt;

	DynamicsTGSContext& mContext;

	PX_NOCOPY(SetupSolverConstraintsTask)

public:

	SetupSolverConstraintsTask(IslandContextStep& islandContext, PxSolverConstraintDesc* contactDescPtr,
		PxsContactManagerOutputIterator& outputs, Dy::ThreadContext& threadContext, PxReal totalDt, DynamicsTGSContext& context) : Cm::Task(context.getContextId()),
		mIslandContext(islandContext), mContactDescPtr(contactDescPtr), mOutputs(outputs), mThreadContext(threadContext), 
		mTotalDt(totalDt), mContext(context)
	{
	}

	virtual const char* getName() const { return "SetupSolverConstraintsTask"; }

	virtual void runInternal()
	{
		Dy::ThreadContext& threadContext = *mIslandContext.mThreadContext;
		const PxU32 nbBatches = threadContext.numContactConstraintBatches;
		PxConstraintBatchHeader* hdr = mIslandContext.mObjects.constraintBatchHeaders;
		//for (PxU32 a = 0; a < mIslandContext.mArticulationOffset; a += SetupSolverConstraintsSubTask::MaxPerTask)
		for (PxU32 a = 0; a < nbBatches; a += SetupSolverConstraintsSubTask::MaxPerTask)
		{
			const PxU32 nbConstraints = PxMin(nbBatches - a, SetupSolverConstraintsSubTask::MaxPerTask);
			SetupSolverConstraintsSubTask* task = PX_PLACEMENT_NEW(mContext.mTaskPool.allocate(sizeof(SetupSolverConstraintsSubTask)), SetupSolverConstraintsSubTask)
				(mContactDescPtr, hdr + a, nbConstraints, mOutputs, mIslandContext.mStepDt, mTotalDt, mIslandContext.mInvStepDt, mContext.mInvDt, mIslandContext.mBiasCoefficient, mThreadContext, mContext);

			task->setContinuation(mCont);
			task->removeReference();
		}

		const PxU32 articCount = mIslandContext.mCounts.articulations;

		for (PxU32 i = 0; i < articCount; i += PxsCreateArticConstraintsSubTask::NbArticsPerTask)
		{
			const PxU32 nbToProcess = PxMin(articCount - i, PxsCreateArticConstraintsSubTask::NbArticsPerTask);

			PxsCreateArticConstraintsSubTask* task = PX_PLACEMENT_NEW(mContext.getTaskPool().allocate(sizeof(PxsCreateArticConstraintsSubTask)), PxsCreateArticConstraintsSubTask)
				(mThreadContext.mArticulationArray + i, nbToProcess, mContext.mSolverBodyDataPool2.begin(), mContext.mSolverBodyTxInertiaPool.begin(), mThreadContext, mContext, mOutputs,
					mIslandContext);

			task->setContinuation(mCont);
			task->removeReference();
		}
	}
};

class PartitionTask : public Cm::Task
{
	IslandContextStep& mIslandContext;
	const PxSolverConstraintDesc* mContactDescPtr;
	PxTGSSolverBodyVel* mSolverBodyData;
	Dy::ThreadContext& mThreadContext;

	DynamicsTGSContext& mContext;

	PX_NOCOPY(PartitionTask)

public:

	PartitionTask(IslandContextStep& islandContext, PxSolverConstraintDesc* contactDescPtr, PxTGSSolverBodyVel* solverBodyData,
		Dy::ThreadContext& threadContext, DynamicsTGSContext& context) : Cm::Task(context.getContextId()),
		mIslandContext(islandContext), mContactDescPtr(contactDescPtr), mSolverBodyData(solverBodyData), mThreadContext(threadContext),
		mContext(context)
	{
	}

	virtual const char* getName() const { return "PartitionTask"; }

	virtual void runInternal()
	{
		const ArticulationSolverDesc* artics = mThreadContext.getArticulations().begin();

		PxU32 totalDescCount = mThreadContext.contactDescArraySize;

		mThreadContext.mConstraintsPerPartition.forceSize_Unsafe(0);
		mThreadContext.mConstraintsPerPartition.resize(1);
		mThreadContext.mConstraintsPerPartition[0] = 0;

		const PxU32 numArticulations = mThreadContext.getArticulations().size();
		PX_ALLOCA(_eaArticulations, Dy::FeatherstoneArticulation*, numArticulations);
		Dy::FeatherstoneArticulation** eaArticulations = _eaArticulations;
		for(PxU32 i=0;i<numArticulations;i++)
			eaArticulations[i] = artics[i].articulation;

		// PT: maxPartitions = PX_MAX_U32 in PGS. Was there a reason to limit it for TGS?
		const ConstraintPartitionIn in(	reinterpret_cast<PxU8*>(mSolverBodyData), mIslandContext.mCounts.bodies, sizeof(PxTGSSolverBodyVel),
										eaArticulations, numArticulations, mContactDescPtr, totalDescCount, 64, false);
		//args.mBitField = &mThreadContext.mPartitionNormalizationBitmap;	// PT: removed, unused

		ConstraintPartitionOut out(mIslandContext.mObjects.orderedConstraintDescs, mIslandContext.mObjects.tempConstraintDescs, &mThreadContext.mConstraintsPerPartition);

		mThreadContext.mMaxPartitions = partitionContactConstraints(out, in);
		mThreadContext.mNumDifferentBodyConstraints = out.mNumDifferentBodyConstraints;
		mThreadContext.mNumStaticConstraints = out.mNumStaticConstraints;
		mThreadContext.mHasOverflowPartitions = out.mNumOverflowConstraints != 0;

		{
			PxU32 descCount = mThreadContext.mNumDifferentBodyConstraints;
			PxU32 selfConstraintDescCount = mThreadContext.contactDescArraySize - (mThreadContext.mNumDifferentBodyConstraints + mThreadContext.mNumStaticConstraints);

			PxArray<PxU32>& accumulatedConstraintsPerPartition = mThreadContext.mConstraintsPerPartition;

			PxU32 numHeaders = 0;
			PxU32 currentPartition = 0;
			PxU32 maxJ = descCount == 0 ? 0 : accumulatedConstraintsPerPartition[0];

			const PxU32 maxBatchPartition = 0xFFFFFFFF;

			//const PxU32 maxBatchSize2 = args.mEnhancedDeterminism ? 1u : 4u;
			const PxU32 maxBatchSize2 = 4u;

			PxConstraintBatchHeader* batchHeaders = mIslandContext.mObjects.constraintBatchHeaders;

			PxSolverConstraintDesc* orderedConstraints = mIslandContext.mObjects.orderedConstraintDescs;

			PxU32 headersPerPartition = 0;

			//KS - if we have overflowed the partition limit, overflow constraints are pushed
			//into 0th partition. If that's the case, we need to disallow batching of constraints
			//in 0th partition.
			PxU32 maxBatchSize = out.mNumOverflowConstraints == 0 ? maxBatchSize2 : 1;

			for (PxU32 a = 0; a < descCount;)
			{
				PxU32 loopMax = PxMin(maxJ - a, maxBatchSize);
				PxU16 j = 0;
				if (loopMax > 0)
				{
					PxConstraintBatchHeader& header = batchHeaders[numHeaders++];

					j = 1;
					PxSolverConstraintDesc& desc = orderedConstraints[a];
					if (!isArticulationConstraint(desc) && (desc.constraintType == DY_SC_TYPE_RB_CONTACT ||
						desc.constraintType == DY_SC_TYPE_RB_1D) && currentPartition < maxBatchPartition)
					{
						for (; j < loopMax && desc.constraintType == orderedConstraints[a + j].constraintType &&
							!isArticulationConstraint(orderedConstraints[a + j]); ++j);
					}
					header.startIndex = a;
					header.stride = j;
					header.constraintType = desc.constraintType;
					headersPerPartition++;
				}
				if (maxJ == (a + j) && maxJ != descCount)
				{
					//Go to next partition!
					accumulatedConstraintsPerPartition[currentPartition] = headersPerPartition;
					headersPerPartition = 0;
					currentPartition++;
					maxJ = accumulatedConstraintsPerPartition[currentPartition];
					maxBatchSize = maxBatchSize2;
				}
				a += j;
			}
			if (descCount)
				accumulatedConstraintsPerPartition[currentPartition] = headersPerPartition;

			accumulatedConstraintsPerPartition.forceSize_Unsafe(mThreadContext.mMaxPartitions);

			for (PxU32 a = 0; a < selfConstraintDescCount; ++a)
			{
				PxConstraintBatchHeader& header = batchHeaders[numHeaders++];
				header.startIndex = a + descCount;
				header.stride = 1;
				header.constraintType = DY_SC_TYPE_EXT_1D;
			}

			mThreadContext.numContactConstraintBatches = numHeaders;
		}
	}
};

class ParallelSolveTask : public Cm::Task
{
	IslandContextStep& mIslandContext;
	const SolverIslandObjectsStep& mObjects;
	const PxsIslandIndices& mCounts;
	PxReal mTotalDt;
	ThreadContext& mThreadContext;

	DynamicsTGSContext& mContext;

	PX_NOCOPY(ParallelSolveTask)

public:

	ParallelSolveTask(IslandContextStep& islandContext, const SolverIslandObjectsStep& objects, const PxsIslandIndices& counts, PxReal totalDt, ThreadContext& threadContext,
		DynamicsTGSContext& context) : Cm::Task(context.getContextId()),
		mIslandContext(islandContext), mObjects(objects), mCounts(counts), mTotalDt(totalDt), mThreadContext(threadContext), mContext(context)
	{
	}

	virtual const char* getName() const { return "ParallelSolveTask"; }

	virtual void runInternal()
	{
		mContext.iterativeSolveIslandParallel(mObjects, mCounts, mThreadContext, mIslandContext.mStepDt, mTotalDt, mIslandContext.mPosIters, mIslandContext.mVelIters,
			&mIslandContext.mSharedSolverIndex, &mIslandContext.mSharedRigidBodyIndex, &mIslandContext.mSharedArticulationIndex, &mIslandContext.mSharedGravityIndex,
			&mIslandContext.mSolvedCount, &mIslandContext.mRigidBodyIntegratedCount, &mIslandContext.mArticulationIntegratedCount, &mIslandContext.mGravityIntegratedCount,
			4, 128, mIslandContext.mBiasCoefficient);
	}
};

class SolveIslandTask : public Cm::Task
{
	IslandContextStep& mIslandContext;
	const SolverIslandObjectsStep& mObjects;
	const PxsIslandIndices& mCounts;
	PxReal mTotalDt;
	ThreadContext& mThreadContext;
	DynamicsTGSContext& mContext;

	PX_NOCOPY(SolveIslandTask)

public:

	SolveIslandTask(IslandContextStep& islandContext, const SolverIslandObjectsStep& objects, const PxsIslandIndices& counts, PxReal totalDt, ThreadContext& threadContext,
		DynamicsTGSContext& context) : Cm::Task(context.getContextId()),
		mIslandContext(islandContext), mObjects(objects), mCounts(counts), mTotalDt(totalDt), mThreadContext(threadContext), mContext(context)
	{
	}

	virtual const char* getName() const { return "SolveIslandTask"; }

	virtual void runInternal()
	{
		PxU32 j = 0, i = 0;

		PxU32 numBatches = 0;

		PxU32 currIndex = 0;
		PxU32 totalCount = 0;

		PxSolverConstraintDesc* contactDescBegin = mObjects.orderedConstraintDescs;
		PxConstraintBatchHeader* headers = mObjects.constraintBatchHeaders;

		PxU32 totalPartitions = 0;
		for (PxU32 a = 0; a < mThreadContext.mConstraintsPerPartition.size(); ++a)
		{
			PxU32 endIndex = currIndex + mThreadContext.mConstraintsPerPartition[a];

			PxU32 numBatchesInPartition = 0;
			for (PxU32 b = currIndex; b < endIndex; ++b)
			{
				PxConstraintBatchHeader& _header = headers[b];
				PxU16 stride = _header.stride, newStride = _header.stride;
				PxU32 startIndex = j;
				for (PxU16 c = 0; c < stride; ++c)
				{
					if (getConstraintLength(contactDescBegin[i]) == 0)
					{
						newStride--;
						i++;
					}
					else
					{
						if (i != j)
							contactDescBegin[j] = contactDescBegin[i];
						i++;
						j++;
					}
				}

				if (newStride != 0)
				{
					headers[numBatches].startIndex = startIndex;
					headers[numBatches].stride = newStride;
					PxU8 type = *contactDescBegin[startIndex].constraint;
					if (type == DY_SC_TYPE_STATIC_CONTACT)
					{
						//Check if any block of constraints is classified as type static (single) contact constraint.
						//If they are, iterate over all constraints grouped with it and switch to "dynamic" contact constraint
						//type if there's a dynamic contact constraint in the group.
						for (PxU32 c = 1; c < newStride; ++c)
						{
							if (*contactDescBegin[startIndex + c].constraint == DY_SC_TYPE_RB_CONTACT)
							{
								type = DY_SC_TYPE_RB_CONTACT;
							}
						}
					}

					headers[numBatches].constraintType = type;
					numBatches++;
					numBatchesInPartition++;
				}
			}
			currIndex += mThreadContext.mConstraintsPerPartition[a];
			mThreadContext.mConstraintsPerPartition[totalPartitions] = numBatchesInPartition;
			if (numBatchesInPartition)
				totalPartitions++;
			else if (a == 0)
				mThreadContext.mHasOverflowPartitions = false; //If our first partition is now empty, we have no overflows so clear the overflow flag
			totalCount += numBatchesInPartition;
		}

		currIndex = totalCount;

		processOverflowConstraints(reinterpret_cast<PxU8*>(mContext.mSolverBodyVelPool.begin() + mObjects.solverBodyOffset+1), sizeof(PxTGSSolverBodyVel),
			mCounts.bodies, mThreadContext.getArticulations().begin(), mThreadContext.getArticulations().size(), contactDescBegin,
			mThreadContext.mHasOverflowPartitions ? mThreadContext.mConstraintsPerPartition[0] : 0);

		//Decision whether to spawn multi-threaded solver or single-threaded solver...

		mThreadContext.mConstraintsPerPartition.forceSize_Unsafe(totalPartitions);
		mThreadContext.numContactConstraintBatches = totalCount;

		PxU32 maxLinks = 0;
		for (PxU32 a = 0; a < mCounts.articulations; ++a)
		{
			ArticulationSolverDesc& desc = mThreadContext.getArticulations()[a];
			maxLinks = PxMax(maxLinks, PxU32(desc.linkCount));
		}

		mThreadContext.mDeltaV.forceSize_Unsafe(0);
		mThreadContext.mDeltaV.reserve(maxLinks);
		mThreadContext.mDeltaV.forceSize_Unsafe(maxLinks);

		mThreadContext.mZVector.forceSize_Unsafe(0);
		mThreadContext.mZVector.reserve(maxLinks);
		mThreadContext.mZVector.forceSize_Unsafe(maxLinks);

		SolverContext cache;
		cache.deltaV = mThreadContext.mDeltaV.begin();
		
		if (mThreadContext.mConstraintsPerPartition.size())
		{
			const PxU32 threadCount = getTaskManager()->getCpuDispatcher()->getWorkerCount();

			PxU32 nbHeadersPerPartition;
			
			if (mThreadContext.mHasOverflowPartitions)
			{
				// jcarius: mitigating potential divide-by-zero problem. It's unclear whether size originally includes
				// the overflow partition or not.
				const PxU32 size = mThreadContext.mConstraintsPerPartition.size();
				const PxU32 nbPartitionsMinusOverflow = (size > 1) ? (size - 1) : 1;

				PxU32 nbConstraintsMinusOverflow = currIndex - mThreadContext.mConstraintsPerPartition[0];
				nbHeadersPerPartition = ((nbConstraintsMinusOverflow + nbPartitionsMinusOverflow - 1) / nbPartitionsMinusOverflow);
			}
			else
				nbHeadersPerPartition = ((currIndex + mThreadContext.mConstraintsPerPartition.size() - 1) / mThreadContext.mConstraintsPerPartition.size());

			const PxU32 NbBatchesPerThread = 8;
			//const PxU32 NbBatchesPerThread = 4;

			const PxU32 nbIdealThreads = (nbHeadersPerPartition + NbBatchesPerThread-1) / NbBatchesPerThread;

			if (threadCount < 2 || nbIdealThreads < 2) // not great if we have many articulations but no contact constraints => PX-4708
				mContext.iterativeSolveIsland(mObjects, mCounts, mThreadContext, mIslandContext.mStepDt, mIslandContext.mInvStepDt, mTotalDt,
					mIslandContext.mPosIters, mIslandContext.mVelIters, cache, mIslandContext.mBiasCoefficient);
			else
			{
				mIslandContext.mSharedSolverIndex = 0;
				mIslandContext.mSolvedCount = 0;
				mIslandContext.mSharedRigidBodyIndex = 0;
				mIslandContext.mRigidBodyIntegratedCount = 0;
				mIslandContext.mSharedArticulationIndex = 0;
				mIslandContext.mArticulationIntegratedCount = 0;
				mIslandContext.mSharedGravityIndex = 0;
				mIslandContext.mGravityIntegratedCount = 0;

				PxU32 nbThreads = PxMin(threadCount, nbIdealThreads);

				ParallelSolveTask* tasks = reinterpret_cast<ParallelSolveTask*>(mContext.getTaskPool().allocate(sizeof(ParallelSolveTask)*nbThreads));

				for (PxU32 a = 0; a < nbThreads; ++a)
				{
					PX_PLACEMENT_NEW(&tasks[a], ParallelSolveTask)(mIslandContext, mObjects, mCounts, mTotalDt, mThreadContext, mContext);
					tasks[a].setContinuation(mCont);
					tasks[a].removeReference();
				}
			}
		}
		else
		{
			mContext.iterativeSolveIsland(mObjects, mCounts, mThreadContext, mIslandContext.mStepDt,
				mIslandContext.mInvStepDt, mTotalDt, mIslandContext.mPosIters, mIslandContext.mVelIters, cache,
				mIslandContext.mBiasCoefficient);
		}
	}
};

class EndIslandTask : public Cm::Task
{
	ThreadContext& mThreadContext;
	DynamicsTGSContext& mContext;

	PX_NOCOPY(EndIslandTask)

public:

	EndIslandTask(ThreadContext& threadContext, DynamicsTGSContext& context) : Cm::Task(context.getContextId()),
		mThreadContext(threadContext), mContext(context)
	{
	}

	virtual const char* getName() const { return "EndIslandTask"; }

	virtual void runInternal()
	{
		mContext.endIsland(mThreadContext);
	}
};

class FinishSolveIslandTask : public Cm::Task
{
	ThreadContext& mThreadContext;
	const SolverIslandObjectsStep& mObjects;
	const PxsIslandIndices& mCounts;
	IG::SimpleIslandManager& mIslandManager;

	DynamicsTGSContext& mContext;

	PX_NOCOPY(FinishSolveIslandTask)

public:

	FinishSolveIslandTask(ThreadContext& threadContext, const SolverIslandObjectsStep& objects,
		const PxsIslandIndices& counts, IG::SimpleIslandManager& islandManager, DynamicsTGSContext& context) : Cm::Task(context.getContextId()),
		mThreadContext(threadContext), mObjects(objects), mCounts(counts), mIslandManager(islandManager), mContext(context)
	{
	}

	virtual const char* getName() const { return "FinishSolveIslandTask"; }

	virtual void runInternal()
	{
		mContext.finishSolveIsland(mThreadContext, mObjects, mCounts, mIslandManager, mCont);
	}
};

void DynamicsTGSContext::iterativeSolveIsland(const SolverIslandObjectsStep& objects, const PxsIslandIndices& counts, ThreadContext& mThreadContext,
	PxReal stepDt, PxReal invStepDt, PxReal totalDt, PxU32 posIters, PxU32 velIters, SolverContext& cache, PxReal biasCoefficient)
{
	PX_PROFILE_ZONE("Dynamics:solveIsland", mContextID);
	PxReal elapsedTime = 0.0f;
	const PxReal recipStepDt = 1.0f/stepDt;

	const PxU32 bodyOffset = objects.solverBodyOffset;

	const bool externalForcesEveryTgsIterationEnabled = mIsExternalForcesEveryTgsIterationEnabled;

	if (mThreadContext.numContactConstraintBatches == 0)
	{
		for (PxU32 i = 0; i < counts.articulations; ++i)
		{
			elapsedTime = 0.0f;
			ArticulationSolverDesc& d = mThreadContext.getArticulations()[i];
			for (PxU32 a = 0; a < posIters; a++)
			{
				if (externalForcesEveryTgsIterationEnabled)
				{
					PX_ASSERT(mThreadContext.mZVector.size() >= d.linkCount);
					FeatherstoneArticulation::applyTgsSubstepForces(d, stepDt, mThreadContext.mZVector.begin());
				}
				d.articulation->solveInternalConstraints(totalDt, stepDt, recipStepDt, false, true, elapsedTime, biasCoefficient,
														 mIsResidualReportingEnabled, mIsExternalForcesEveryTgsIterationEnabled);
				ArticulationPImpl::updateDeltaMotion(d, stepDt, mThreadContext.mDeltaV.begin(), mInvDt);
				elapsedTime += stepDt;
			}

			ArticulationPImpl::saveVelocityTGS(d.articulation, mInvDt);

			d.articulation->concludeInternalConstraints(true);

			for (PxU32 a = 0; a < velIters; ++a)
			{
				d.articulation->solveInternalConstraints(totalDt, stepDt, recipStepDt, true, true, elapsedTime, biasCoefficient,
														 mIsResidualReportingEnabled, mIsExternalForcesEveryTgsIterationEnabled);
			}

			d.articulation->writebackInternalConstraints(true);
		}

		if (externalForcesEveryTgsIterationEnabled)
			integrateBodiesAndApplyGravity(objects, counts.bodies, mSolverBodyVelPool.begin() + bodyOffset, mSolverBodyTxInertiaPool.begin() + bodyOffset, stepDt, posIters);
		else
			integrateBodies(counts.bodies, mSolverBodyVelPool.begin() + bodyOffset, mSolverBodyTxInertiaPool.begin() + bodyOffset, mDt);
		return;
	}

	cache.contactErrorAccumulator = mIsResidualReportingEnabled ? &mThreadContext.getSimStats().contactErrorAccumulator.mPositionIterationErrorAccumulator : NULL;
	cache.isPositionIteration = true;
	for (PxU32 a = 1; a < posIters; a++)
	{
		if (cache.contactErrorAccumulator)
			cache.contactErrorAccumulator->reset();
		if(externalForcesEveryTgsIterationEnabled)
		{
			applySubstepGravity(objects.bodies, *objects.externalAccelerations, counts.bodies, mSolverBodyVelPool.begin() + bodyOffset, stepDt, mSolverBodyTxInertiaPool.begin() + bodyOffset, objects.nodeIndexArray);
			applyArticulationTgsSubstepForces(mThreadContext, counts.articulations, stepDt);
		}

		solveConstraintsIteration(objects.orderedConstraintDescs, objects.constraintBatchHeaders, mThreadContext.numContactConstraintBatches, invStepDt,
			mSolverBodyTxInertiaPool.begin(), elapsedTime, -PX_MAX_F32, cache);
		integrateBodies(counts.bodies, mSolverBodyVelPool.begin() + bodyOffset, mSolverBodyTxInertiaPool.begin() + bodyOffset, stepDt);

		for (PxU32 i = 0; i < counts.articulations; ++i)
		{
			ArticulationSolverDesc& d = mThreadContext.getArticulations()[i];
			d.articulation->solveInternalConstraints(totalDt, stepDt, recipStepDt, false, true, elapsedTime, biasCoefficient,
													 mIsResidualReportingEnabled, mIsExternalForcesEveryTgsIterationEnabled);
		}

		stepArticulations(mThreadContext, counts, stepDt, mInvDt);
		elapsedTime += stepDt;
	}

	if (cache.contactErrorAccumulator)
		cache.contactErrorAccumulator->reset();
	if(externalForcesEveryTgsIterationEnabled)
	{
		applySubstepGravity(objects.bodies, *objects.externalAccelerations, counts.bodies, mSolverBodyVelPool.begin() + bodyOffset, stepDt, mSolverBodyTxInertiaPool.begin() + bodyOffset, objects.nodeIndexArray);
		applyArticulationTgsSubstepForces(mThreadContext, counts.articulations, stepDt);
	}

	solveConcludeConstraintsIteration<false>(objects.orderedConstraintDescs, objects.constraintBatchHeaders, mThreadContext.numContactConstraintBatches,
		mSolverBodyTxInertiaPool.begin(), elapsedTime, cache, 0);

	for (PxU32 i = 0; i < counts.articulations; ++i)
	{
		ArticulationSolverDesc& d = mThreadContext.getArticulations()[i];
		d.articulation->solveInternalConstraints(totalDt, stepDt, recipStepDt, false, true, elapsedTime, biasCoefficient,
												 mIsResidualReportingEnabled, mIsExternalForcesEveryTgsIterationEnabled);
		d.articulation->concludeInternalConstraints(true);
	}

	elapsedTime += stepDt;

	const PxReal invDt = mInvDt;

	integrateBodies(counts.bodies, mSolverBodyVelPool.begin() + bodyOffset, mSolverBodyTxInertiaPool.begin() + bodyOffset, 
		stepDt);

	stepArticulations(mThreadContext, counts, stepDt, mInvDt);

	for(PxU32 a = 0; a < counts.articulations; ++a)
	{
		Dy::ArticulationSolverDesc& desc = mThreadContext.getArticulations()[a];

		//ArticulationPImpl::updateDeltaMotion(desc, stepDt, mThreadContext.mDeltaV.begin());
		ArticulationPImpl::saveVelocityTGS(desc.articulation, invDt);
	}

	cache.contactErrorAccumulator = mIsResidualReportingEnabled ? &mThreadContext.getSimStats().contactErrorAccumulator.mVelocityIterationErrorAccumulator : NULL;
	cache.isPositionIteration = false;
	for (PxU32 a = 0; a < velIters; ++a)
	{
		if (cache.contactErrorAccumulator)
			cache.contactErrorAccumulator->reset();
		solveConstraintsIteration(objects.orderedConstraintDescs, objects.constraintBatchHeaders, mThreadContext.numContactConstraintBatches, invStepDt,
			mSolverBodyTxInertiaPool.begin(), elapsedTime, /*-PX_MAX_F32*/0.f, cache);
		for (PxU32 i = 0; i < counts.articulations; ++i)
		{
			ArticulationSolverDesc& d = mThreadContext.getArticulations()[i];
			d.articulation->solveInternalConstraints(totalDt, stepDt, recipStepDt, true, true, elapsedTime, biasCoefficient,
													 mIsResidualReportingEnabled, mIsExternalForcesEveryTgsIterationEnabled);
		}
	}

	writebackConstraintsIteration(objects.constraintBatchHeaders, objects.orderedConstraintDescs, mThreadContext.numContactConstraintBatches, cache);
	for (PxU32 i = 0; i < counts.articulations; ++i)
	{
		ArticulationSolverDesc& d = mThreadContext.getArticulations()[i];
		d.articulation->writebackInternalConstraints(true);
	}
}

void DynamicsTGSContext::applySubstepGravityParallel(const SolverIslandObjectsStep& objects, PxTGSSolverBodyVel* solverVels, const PxU32 bodyOffset, PxReal stepDt,
	const PxU32 nbBodies, PxU32& startGravityIdx, PxU32& nbGravityRemaining, PxU32& targetGravityProgressCount,
	PxI32* gravityProgressCount, PxI32* gravityCounts, PxU32 unrollSize)
{
	PxU32 gravityStartIdx = startGravityIdx - targetGravityProgressCount;

	PxU32 nbGravity = 0;
	while (gravityStartIdx < nbBodies)
	{
		const PxU32 nbToGravity = PxMin(nbBodies - gravityStartIdx, nbGravityRemaining);

		applySubstepGravity(objects.bodies + gravityStartIdx, *objects.externalAccelerations, nbToGravity,
			solverVels + bodyOffset + gravityStartIdx, stepDt, mSolverBodyTxInertiaPool.begin() + bodyOffset + gravityStartIdx, &objects.nodeIndexArray[gravityStartIdx]);

		nbGravityRemaining -= nbToGravity;
		startGravityIdx += nbToGravity;
		gravityStartIdx += nbToGravity;

		nbGravity += nbToGravity;

		if (nbGravityRemaining == 0)
		{
			startGravityIdx = PxU32(PxAtomicAdd(gravityCounts, PxI32(unrollSize))) - unrollSize;
			nbGravityRemaining = unrollSize;
			gravityStartIdx = startGravityIdx - targetGravityProgressCount;
		}
	}

	if (nbGravity)
		PxAtomicAdd(gravityProgressCount, PxI32(nbGravity));

	targetGravityProgressCount += nbBodies;
}

void DynamicsTGSContext::applyArticulationSubstepGravityParallel(
    PxU32& startArticulationIdx, PxU32& targetArticulationProgressCount, PxI32* articulationProgressCount,
    PxI32* articulationIntegrationCounts, ArticulationSolverDesc* articulationDescs, PxU32 nbArticulations,
    PxReal stepDt, ThreadContext& threadContext)
{
	PxU32 artIcStartIdx = startArticulationIdx - targetArticulationProgressCount;
	PxU32 nbArticsProcessed = 0;
	while(artIcStartIdx < nbArticulations)
	{
		ArticulationSolverDesc& d = articulationDescs[artIcStartIdx];
		FeatherstoneArticulation::applyTgsSubstepForces(d, stepDt, threadContext.mZVector.begin());

		nbArticsProcessed++;

		startArticulationIdx = PxU32(PxAtomicAdd(articulationIntegrationCounts, PxI32(1))) - 1;
		artIcStartIdx = startArticulationIdx - targetArticulationProgressCount;
	}

	if(nbArticsProcessed)
		PxAtomicAdd(articulationProgressCount, PxI32(nbArticsProcessed));

	targetArticulationProgressCount += nbArticulations;
}

void DynamicsTGSContext::iterativeSolveIslandParallel(const SolverIslandObjectsStep& objects, const PxsIslandIndices& counts, ThreadContext& mThreadContext,
	PxReal stepDt, PxReal totalDt, PxU32 posIters, PxU32 velIters, PxI32* solverCounts, PxI32* integrationCounts, PxI32* articulationIntegrationCounts, PxI32* gravityCounts,
	PxI32* solverProgressCount, PxI32* integrationProgressCount, PxI32* articulationProgressCount, PxI32* gravityProgressCount, PxU32 solverUnrollSize, PxU32 integrationUnrollSize,
	PxReal biasCoefficient)
{
	PX_PROFILE_ZONE("Dynamics:solveIslandParallel", mContextID);
	Dy::ThreadContext& threadContext = *getThreadContext();
	PxU32 startSolveIdx = PxU32(PxAtomicAdd(solverCounts, PxI32(solverUnrollSize))) - solverUnrollSize;
	PxU32 nbSolveRemaining = solverUnrollSize;
	
	PxU32 startIntegrateIdx = PxU32(PxAtomicAdd(integrationCounts, PxI32(integrationUnrollSize))) - integrationUnrollSize;
	PxU32 nbIntegrateRemaining = integrationUnrollSize;

	PxU32 startGravityIdx = PxU32(PxAtomicAdd(gravityCounts, PxI32(integrationUnrollSize))) - integrationUnrollSize;
	PxU32 nbGravityRemaining = integrationUnrollSize;

	//For now, just do articulations 1 at a time. Might need to tweak this later depending on performance
	PxU32 startArticulationIdx = PxU32(PxAtomicAdd(articulationIntegrationCounts, PxI32(1))) - 1;

	PxU32 targetSolverProgressCount = 0, targetIntegrationProgressCount = 0, targetArticulationProgressCount = 0, targetGravityProgressCount = 0;

	const PxU32 nbSolverBatches = mThreadContext.numContactConstraintBatches;
	const PxU32 nbBodies = counts.bodies;// + mKinematicCount;
	const PxU32 nbArticulations = counts.articulations;

	PxSolverConstraintDesc* contactDescs = objects.orderedConstraintDescs;
	PxConstraintBatchHeader* batchHeaders = objects.constraintBatchHeaders;
	
	PxTGSSolverBodyVel* solverVels = mSolverBodyVelPool.begin();
	PxTGSSolverBodyTxInertia* solverTxInertias = mSolverBodyTxInertiaPool.begin();

	PxU32* constraintsPerPartitions = mThreadContext.mConstraintsPerPartition.begin();
	const PxU32 nbPartitions = mThreadContext.mConstraintsPerPartition.size();

	const PxU32 bodyOffset = objects.solverBodyOffset;

	threadContext.mDeltaV.reserve(mThreadContext.mDeltaV.size());
	threadContext.mZVector.resizeUninitialized(mThreadContext.mZVector.size());
	
	SolverContext cache;
	cache.deltaV = threadContext.mDeltaV.begin();
	cache.contactErrorAccumulator = isResidualReportingEnabled() ? &mThreadContext.getSimStats().contactErrorAccumulator.mPositionIterationErrorAccumulator : NULL;
	cache.isPositionIteration = true;

	PxReal elapsedTime = 0.0f;

	PxReal invStepDt = 1.0f/ stepDt;

	const bool overflow = mThreadContext.mHasOverflowPartitions;

	PxU32 iterCount = 0;

	const bool externalForcesEveryTgsIterationEnabled = mIsExternalForcesEveryTgsIterationEnabled;

	for (PxU32 a = 1; a < posIters; ++a)
	{
		// make sure all work from the previous position iteration is complete
		WAIT_FOR_PROGRESS(integrationProgressCount, PxI32(targetIntegrationProgressCount));
		WAIT_FOR_PROGRESS(articulationProgressCount, PxI32(targetArticulationProgressCount));
		if (cache.contactErrorAccumulator)
			cache.contactErrorAccumulator->reset();
		if(externalForcesEveryTgsIterationEnabled)
		{
			applySubstepGravityParallel(objects, solverVels, bodyOffset, stepDt, nbBodies, startGravityIdx,
			                            nbGravityRemaining, targetGravityProgressCount, gravityProgressCount,
			                            gravityCounts, integrationUnrollSize);
			applyArticulationSubstepGravityParallel(startArticulationIdx, targetArticulationProgressCount,
			                                        articulationProgressCount, articulationIntegrationCounts,
			                                        mThreadContext.getArticulations().begin(), nbArticulations, stepDt,
			                                        threadContext);

			// Make all threads wait until gravity is applied to all articulations and all bodies.
			// The target progress counters have been incremented inside the substep gravity functions.
			WAIT_FOR_PROGRESS(articulationProgressCount, PxI32(targetArticulationProgressCount));
			WAIT_FOR_PROGRESS(gravityProgressCount, PxI32(targetGravityProgressCount));
		}

		PxU32 offset = 0;
		for (PxU32 b = 0; b < nbPartitions; ++b)
		{
			WAIT_FOR_PROGRESS(solverProgressCount, PxI32(targetSolverProgressCount));
			//Find the startIdx in the partition to process
			PxU32 startIdx = startSolveIdx - targetSolverProgressCount;

			const PxU32 nbBatches = constraintsPerPartitions[b];

			PxU32 nbSolved = 0;

			while (startIdx < nbBatches)
			{
				const PxU32 nbToSolve = PxMin(nbBatches - startIdx, nbSolveRemaining);

				if (b == 0 && overflow)
					parallelSolveConstraints<true>(contactDescs, batchHeaders + startIdx + offset, nbToSolve,
						solverTxInertias, elapsedTime, -PX_MAX_F32, cache, iterCount);
				else
					parallelSolveConstraints<false>(contactDescs, batchHeaders + startIdx + offset, nbToSolve,
						solverTxInertias, elapsedTime, -PX_MAX_F32, cache, iterCount);

				nbSolveRemaining -= nbToSolve;
				startSolveIdx += nbToSolve;
				startIdx += nbToSolve;

				nbSolved += nbToSolve;

				if (nbSolveRemaining == 0)
				{
					startSolveIdx = PxU32(PxAtomicAdd(solverCounts, PxI32(solverUnrollSize))) - solverUnrollSize;
					nbSolveRemaining = solverUnrollSize;
					startIdx = startSolveIdx - targetSolverProgressCount;
				}
			}

			if (nbSolved)
				PxAtomicAdd(solverProgressCount, PxI32(nbSolved));

			targetSolverProgressCount += nbBatches;
			offset += nbBatches;
		}

		iterCount++;

		WAIT_FOR_PROGRESS(solverProgressCount, PxI32(targetSolverProgressCount));

		{
			PxU32 integStartIdx = startIntegrateIdx - targetIntegrationProgressCount;

			PxU32 nbIntegrated = 0;
			while (integStartIdx < nbBodies)
			{
				const PxU32 nbToIntegrate = PxMin(nbBodies - integStartIdx, nbIntegrateRemaining);

				parallelIntegrateBodies(solverVels + integStartIdx + bodyOffset, solverTxInertias + integStartIdx + bodyOffset,
					nbToIntegrate, stepDt, iterCount);

				nbIntegrateRemaining -= nbToIntegrate;
				startIntegrateIdx += nbToIntegrate;
				integStartIdx += nbToIntegrate;

				nbIntegrated += nbToIntegrate;

				if (nbIntegrateRemaining == 0)
				{
					startIntegrateIdx = PxU32(PxAtomicAdd(integrationCounts, PxI32(integrationUnrollSize))) - integrationUnrollSize;
					nbIntegrateRemaining = integrationUnrollSize;
					integStartIdx = startIntegrateIdx - targetIntegrationProgressCount;
				}
			}

			if (nbIntegrated)
				PxAtomicAdd(integrationProgressCount, PxI32(nbIntegrated));

			targetIntegrationProgressCount += nbBodies;
		}

		// Note: Articulation internal constraints do not need to wait for rigid bodies integration because they don't need the
		// rigid velocities anymore.

		PxU32 artIcStartIdx = startArticulationIdx - targetArticulationProgressCount;
		PxU32 nbArticsProcessed = 0;
		while (artIcStartIdx < nbArticulations)
		{
			ArticulationSolverDesc& d = mThreadContext.getArticulations()[artIcStartIdx];

			d.articulation->solveInternalConstraints(totalDt, stepDt, invStepDt, false, true, elapsedTime, biasCoefficient,
													 mIsResidualReportingEnabled, mIsExternalForcesEveryTgsIterationEnabled);

			ArticulationPImpl::updateDeltaMotion(d, stepDt, cache.deltaV, mInvDt);

			nbArticsProcessed++;

			startArticulationIdx = PxU32(PxAtomicAdd(articulationIntegrationCounts, PxI32(1))) - 1;
			artIcStartIdx = startArticulationIdx - targetArticulationProgressCount;
		}

		if (nbArticsProcessed)
			PxAtomicAdd(articulationProgressCount, PxI32(nbArticsProcessed));

		targetArticulationProgressCount += nbArticulations;

		elapsedTime += stepDt;
	}

	// The last position iteration with conclude constraint
	{
		WAIT_FOR_PROGRESS(integrationProgressCount, PxI32(targetIntegrationProgressCount));
		WAIT_FOR_PROGRESS(articulationProgressCount, PxI32(targetArticulationProgressCount));

		if (cache.contactErrorAccumulator)
			cache.contactErrorAccumulator->reset();
		if(externalForcesEveryTgsIterationEnabled)
		{
			applySubstepGravityParallel(objects, solverVels, bodyOffset, stepDt, nbBodies, startGravityIdx,
			                            nbGravityRemaining, targetGravityProgressCount, gravityProgressCount,
			                            gravityCounts, integrationUnrollSize);
			applyArticulationSubstepGravityParallel(startArticulationIdx, targetArticulationProgressCount,
			                                        articulationProgressCount, articulationIntegrationCounts,
			                                        mThreadContext.getArticulations().begin(), nbArticulations, stepDt,
			                                        threadContext);

			// Make all threads wait until gravity is applied to all articulations and all bodies.
			// The target progress counters have been incremented inside the substep gravity functions.
			WAIT_FOR_PROGRESS(articulationProgressCount, PxI32(targetArticulationProgressCount));
			WAIT_FOR_PROGRESS(gravityProgressCount, PxI32(targetGravityProgressCount));
		}

		PxU32 offset = 0;
		for (PxU32 b = 0; b < nbPartitions; ++b)
		{
			WAIT_FOR_PROGRESS(solverProgressCount, PxI32(targetSolverProgressCount));
			//Find the startIdx in the partition to process
			PxU32 startIdx = startSolveIdx - targetSolverProgressCount;

			const PxU32 nbBatches = constraintsPerPartitions[b];

			PxU32 nbSolved = 0;
			while (startIdx < nbBatches)
			{
				PxU32 nbToSolve = PxMin(nbBatches - startIdx, nbSolveRemaining);
				if (b == 0 && overflow)
					solveConcludeConstraintsIteration<true>(contactDescs, batchHeaders + startIdx + offset, nbToSolve,
						solverTxInertias, elapsedTime, cache, iterCount);
				else
					solveConcludeConstraintsIteration<false>(contactDescs, batchHeaders + startIdx + offset, nbToSolve,
						solverTxInertias, elapsedTime, cache, iterCount);
				nbSolveRemaining -= nbToSolve;
				startSolveIdx += nbToSolve;
				startIdx += nbToSolve;

				nbSolved += nbToSolve;

				if (nbSolveRemaining == 0)
				{
					startSolveIdx = PxU32(PxAtomicAdd(solverCounts, PxI32(solverUnrollSize))) - solverUnrollSize;
					nbSolveRemaining = solverUnrollSize;
					startIdx = startSolveIdx - targetSolverProgressCount;
				}
			}

			if (nbSolved)
				PxAtomicAdd(solverProgressCount, PxI32(nbSolved));

			targetSolverProgressCount += nbBatches;
			offset += nbBatches;
		}

		iterCount++;

		WAIT_FOR_PROGRESS(solverProgressCount, PxI32(targetSolverProgressCount));

		{
			PxU32 integStartIdx = startIntegrateIdx - targetIntegrationProgressCount;
			PxU32 nbIntegrated = 0;
			while (integStartIdx < nbBodies)
			{
				PxU32 nbToIntegrate = PxMin(nbBodies - integStartIdx, nbIntegrateRemaining);

				parallelIntegrateBodies(solverVels + integStartIdx + bodyOffset, solverTxInertias + integStartIdx + bodyOffset,
					nbToIntegrate, stepDt, iterCount);

				nbIntegrateRemaining -= nbToIntegrate;
				startIntegrateIdx += nbToIntegrate;
				integStartIdx += nbToIntegrate;

				nbIntegrated += nbToIntegrate;

				if (nbIntegrateRemaining == 0)
				{
					startIntegrateIdx = PxU32(PxAtomicAdd(integrationCounts, PxI32(integrationUnrollSize))) - integrationUnrollSize;
					nbIntegrateRemaining = integrationUnrollSize;
					integStartIdx = startIntegrateIdx - targetIntegrationProgressCount;
				}
			}

			if (nbIntegrated)
				PxAtomicAdd(integrationProgressCount, PxI32(nbIntegrated));

			targetIntegrationProgressCount += nbBodies;
		}

		PxU32 artIcStartIdx = startArticulationIdx - targetArticulationProgressCount;

		const PxReal invDt = mInvDt;
		PxU32 nbArticsProcessed = 0;
		while (artIcStartIdx < nbArticulations)
		{
			ArticulationSolverDesc& d = mThreadContext.getArticulations()[artIcStartIdx];

			d.articulation->solveInternalConstraints(totalDt, stepDt, invStepDt, false, true, elapsedTime, biasCoefficient, mIsResidualReportingEnabled, mIsExternalForcesEveryTgsIterationEnabled);

			d.articulation->concludeInternalConstraints(true);

			ArticulationPImpl::updateDeltaMotion(d, stepDt, cache.deltaV, mInvDt);
			ArticulationPImpl::saveVelocityTGS(d.articulation, invDt);

			nbArticsProcessed++;

			startArticulationIdx = PxU32(PxAtomicAdd(articulationIntegrationCounts, PxI32(1))) - 1;
			artIcStartIdx = startArticulationIdx - targetArticulationProgressCount;
		}

		if (nbArticsProcessed)
			PxAtomicAdd(articulationProgressCount, PxI32(nbArticsProcessed));

		targetArticulationProgressCount += nbArticulations;

		elapsedTime += stepDt;

		putThreadContext(&threadContext);
	}


	WAIT_FOR_PROGRESS(integrationProgressCount, PxI32(targetIntegrationProgressCount));
	WAIT_FOR_PROGRESS(articulationProgressCount, PxI32(targetArticulationProgressCount));

	cache.contactErrorAccumulator = isResidualReportingEnabled() ? &mThreadContext.getSimStats().contactErrorAccumulator.mVelocityIterationErrorAccumulator : NULL;
	cache.isPositionIteration = false;
	// Velocity Iterations. It's legal to skip them (velIters == 0).
	for (PxU32 a = 0; a < velIters; ++a)
	{
		WAIT_FOR_PROGRESS(solverProgressCount, PxI32(targetSolverProgressCount));
		if (cache.contactErrorAccumulator)
			cache.contactErrorAccumulator->reset();

		PxU32 offset = 0;
		for (PxU32 b = 0; b < nbPartitions; ++b)
		{
			WAIT_FOR_PROGRESS(solverProgressCount, PxI32(targetSolverProgressCount));

			//Find the startIdx in the partition to process
			PxU32 startIdx = startSolveIdx - targetSolverProgressCount;

			const PxU32 nbBatches = constraintsPerPartitions[b];

			PxU32 nbSolved = 0;
			while (startIdx < nbBatches)
			{
				PxU32 nbToSolve = PxMin(nbBatches - startIdx, nbSolveRemaining);
				if (b == 0 && overflow)
					parallelSolveConstraints<true>(contactDescs, batchHeaders + startIdx + offset, nbToSolve,
						solverTxInertias, elapsedTime, 0.f/*-PX_MAX_F32*/, cache, iterCount);
				else
					parallelSolveConstraints<false>(contactDescs, batchHeaders + startIdx + offset, nbToSolve,
						solverTxInertias, elapsedTime, 0.f/*-PX_MAX_F32*/, cache, iterCount);

				nbSolveRemaining -= nbToSolve;
				startSolveIdx += nbToSolve;
				startIdx += nbToSolve;

				nbSolved += nbToSolve;

				if (nbSolveRemaining == 0)
				{
					startSolveIdx = PxU32(PxAtomicAdd(solverCounts, PxI32(solverUnrollSize))) - solverUnrollSize;
					nbSolveRemaining = solverUnrollSize;

					startIdx = startSolveIdx - targetSolverProgressCount;
				}
			}

			if (nbSolved)
				PxAtomicAdd(solverProgressCount, PxI32(nbSolved));

			targetSolverProgressCount += nbBatches;
			offset += nbBatches;
		}

		WAIT_FOR_PROGRESS(solverProgressCount, PxI32(targetSolverProgressCount));

		PxU32 artIcStartIdx = startArticulationIdx - targetArticulationProgressCount;

		PxU32 nbArticsProcessed = 0;
		while (artIcStartIdx < nbArticulations)
		{
			ArticulationSolverDesc& d = mThreadContext.getArticulations()[artIcStartIdx];

			d.articulation->solveInternalConstraints(totalDt, stepDt, invStepDt, true, true, elapsedTime, biasCoefficient,
													 mIsResidualReportingEnabled, mIsExternalForcesEveryTgsIterationEnabled);

			nbArticsProcessed++;

			startArticulationIdx = PxU32(PxAtomicAdd(articulationIntegrationCounts, PxI32(1))) - 1;
			artIcStartIdx = startArticulationIdx - targetArticulationProgressCount;
		}

		if (nbArticsProcessed)
			PxAtomicAdd(articulationProgressCount, PxI32(nbArticsProcessed));

		targetArticulationProgressCount += nbArticulations;

		iterCount++;

		WAIT_FOR_PROGRESS(articulationProgressCount, PxI32(targetArticulationProgressCount));
	}

	//Write back constraints...
	{		
		// Rigids
		{
			//Find the startIdx in the partition to process
			PxU32 startIdx = startSolveIdx - targetSolverProgressCount;

			const PxU32 nbBatches = nbSolverBatches;

			while (startIdx < nbBatches)
			{
				PxU32 nbToSolve = PxMin(nbBatches - startIdx, nbSolveRemaining);
				parallelWritebackConstraintsIteration(contactDescs, batchHeaders + startIdx, nbToSolve, cache);
				nbSolveRemaining -= nbToSolve;
				startSolveIdx += nbToSolve;
				startIdx += nbToSolve;

				if (nbSolveRemaining == 0)
				{
					startSolveIdx = PxU32(PxAtomicAdd(solverCounts, PxI32(solverUnrollSize))) - solverUnrollSize;
					nbSolveRemaining = solverUnrollSize;
					startIdx = startSolveIdx - targetSolverProgressCount;
				}
			}

			// No need here to atomically add to solverProgressCount because this is the last thing we do during solve
		}

		// Articulations
		{
			PxU32 artIcStartIdx = startArticulationIdx - targetArticulationProgressCount;

			while(artIcStartIdx < nbArticulations)
			{
				ArticulationSolverDesc& d = mThreadContext.getArticulations()[artIcStartIdx];

				d.articulation->writebackInternalConstraints(true);

				startArticulationIdx = PxU32(PxAtomicAdd(articulationIntegrationCounts, PxI32(1))) - 1;
				artIcStartIdx = startArticulationIdx - targetArticulationProgressCount;
			}

			// No need here to atomically add to articulationProgressCount because this is the last thing we do during solve
		}

		// There is no need to WAIT_FOR_PROGRESS here anymore. Once a thread has reached this point, it has processed
		// all work it was assigned and we can let it return to pick up another task. One thread returning here does not
		// mean, however, that all work is done because other threads may still work on their rigids or articulations
		// they've been assigned via the startSolveIdx and startArticulationIdx atomic calls.
	}
}

class CopyBackTask : public Cm::Task
{
	const SolverIslandObjectsStep& mObjects;
	PxTGSSolverBodyVel* mVels;
	PxTGSSolverBodyTxInertia* mTxInertias;
	PxTGSSolverBodyData* mSolverBodyDatas;
	const PxReal mInvDt;
	IG::IslandSim& mIslandSim;
	const PxU32 mStartIdx;
	const PxU32 mEndIdx;
	DynamicsTGSContext& mContext;

	PX_NOCOPY(CopyBackTask)

public:

	CopyBackTask(const SolverIslandObjectsStep& objects,
		PxTGSSolverBodyVel* vels, PxTGSSolverBodyTxInertia* txInertias,
		PxTGSSolverBodyData* solverBodyDatas, PxReal invDt, IG::IslandSim& islandSim,
		PxU32 startIdx, PxU32 endIdx, DynamicsTGSContext& context) : Cm::Task(context.getContextId()),
		mObjects(objects), mVels(vels), mTxInertias(txInertias), mSolverBodyDatas(solverBodyDatas), mInvDt(invDt),
		mIslandSim(islandSim), mStartIdx(startIdx), mEndIdx(endIdx), mContext(context)
	{
	}

	virtual const char* getName() const { return "CopyBackTask"; }

	virtual void runInternal()
	{
		mContext.copyBackBodies(mObjects, mVels, mTxInertias, mSolverBodyDatas, mInvDt, mIslandSim, mStartIdx, mEndIdx);
	}
};

class UpdateArticTask : public Cm::Task
{
	Dy::ThreadContext& mThreadContext;
	PxU32 mStartIdx;
	PxU32 mEndIdx;
	PxReal mDt;
	DynamicsTGSContext& mContext;

	PX_NOCOPY(UpdateArticTask)

public:

	UpdateArticTask(Dy::ThreadContext& threadContext, PxU32 startIdx, PxU32 endIdx,	PxReal dt, DynamicsTGSContext& context) :
		Cm::Task(context.getContextId()),
		mThreadContext(threadContext), mStartIdx(startIdx), mEndIdx(endIdx), mDt(dt), mContext(context)
	{
	}

	virtual const char* getName() const { return "UpdateArticTask"; }

	virtual void runInternal()
	{
		mContext.updateArticulations(mThreadContext, mStartIdx, mEndIdx, mDt);
	}
};

void DynamicsTGSContext::finishSolveIsland(ThreadContext& mThreadContext, const SolverIslandObjectsStep& objects,
	const PxsIslandIndices& counts, IG::SimpleIslandManager& islandManager, PxBaseTask* continuation)
{
	mThreadContext.mConstraintBlockManager.reset();
	mThreadContext.mConstraintBlockStream.reset();

	const PxU32 NbBodiesPerTask = 512;

	for (PxU32 a = 0; a < counts.bodies; a += NbBodiesPerTask)
	{
		CopyBackTask* task = PX_PLACEMENT_NEW(mTaskPool.allocate(sizeof(CopyBackTask)), CopyBackTask)
			(objects, mSolverBodyVelPool.begin() + objects.solverBodyOffset,
				mSolverBodyTxInertiaPool.begin() + objects.solverBodyOffset, mSolverBodyDataPool2.begin() + objects.solverBodyOffset,
				mInvDt, islandManager.getAccurateIslandSim(), a, PxMin(a + NbBodiesPerTask, counts.bodies),*this);

		task->setContinuation(continuation);
		task->removeReference();
	}

	const PxU32 NbArticsPerTask = 64;

	for (PxU32 a = 0; a < counts.articulations; a += NbArticsPerTask)
	{
		UpdateArticTask* task = PX_PLACEMENT_NEW(mTaskPool.allocate(sizeof(UpdateArticTask)), UpdateArticTask)
			(mThreadContext, a, PxMin(counts.articulations, a+NbArticsPerTask), mDt, *this);

		task->setContinuation(continuation);
		task->removeReference();
	}
}

void DynamicsTGSContext::endIsland(ThreadContext& mThreadContext)
{
	putThreadContext(&mThreadContext);
}

void DynamicsTGSContext::solveIsland(const SolverIslandObjectsStep& objects,
	const PxsIslandIndices& counts,
	PxU32 solverBodyOffset,
	IG::SimpleIslandManager& islandManager,
	PxU32* bodyRemapTable, PxsMaterialManager* /*materialManager*/,
	PxsContactManagerOutputIterator& iterator,
	PxBaseTask* continuation)
{
	ThreadContext& mThreadContext = *getThreadContext();

	IslandContextStep& islandContext = *reinterpret_cast<IslandContextStep*>(mTaskPool.allocate(sizeof(IslandContextStep)));
	islandContext.mThreadContext = &mThreadContext;
	islandContext.mCounts = counts;
	islandContext.mObjects = objects;
	islandContext.mPosIters = 0;
	islandContext.mVelIters = 0;
	islandContext.mObjects.solverBodyOffset = solverBodyOffset;

	prepareBodiesAndConstraints(islandContext.mObjects, islandManager, islandContext);

	////Create task chain...
	SetupDescsTask* descTask = PX_PLACEMENT_NEW(mTaskPool.allocate(sizeof(SetupDescsTask)), SetupDescsTask)(islandContext, islandContext.mObjects, 
		bodyRemapTable, solverBodyOffset, iterator, *this);
	PreIntegrateTask* intTask = PX_PLACEMENT_NEW(mTaskPool.allocate(sizeof(PreIntegrateTask)), PreIntegrateTask)(islandContext.mObjects.bodyCoreArray, islandContext.mObjects.bodies,
		mSolverBodyVelPool.begin() + solverBodyOffset,
		mSolverBodyTxInertiaPool.begin() + solverBodyOffset, mSolverBodyDataPool2.begin() + solverBodyOffset,
		mThreadContext.mNodeIndexArray, islandContext.mCounts.bodies, mGravity, mDt, islandContext.mPosIters,
		islandContext.mVelIters, *this);
	SetupArticulationTask* articTask = PX_PLACEMENT_NEW(mTaskPool.allocate(sizeof(SetupArticulationTask)), SetupArticulationTask)(islandContext, mGravity, 
		mDt, islandContext.mPosIters, islandContext.mVelIters, *this);
	SetStepperTask* stepperTask = PX_PLACEMENT_NEW(mTaskPool.allocate(sizeof(SetStepperTask)), SetStepperTask)(islandContext, *this);
	SetupArticulationInternalConstraintsTask* articConTask = PX_PLACEMENT_NEW(mTaskPool.allocate(sizeof(SetupArticulationInternalConstraintsTask)), SetupArticulationInternalConstraintsTask)
		(islandContext, mDt, mInvDt, *this);
	PartitionTask* partitionTask = PX_PLACEMENT_NEW(mTaskPool.allocate(sizeof(PartitionTask)), PartitionTask)
		(islandContext, islandContext.mObjects.constraintDescs, mSolverBodyVelPool.begin()+solverBodyOffset+1, mThreadContext,
			*this);
	SetupSolverConstraintsTask* constraintTask = PX_PLACEMENT_NEW(mTaskPool.allocate(sizeof(SetupSolverConstraintsTask)), SetupSolverConstraintsTask)
		(islandContext, islandContext.mObjects.orderedConstraintDescs, iterator, mThreadContext,
			mDt, *this);

	SolveIslandTask* solveTask = PX_PLACEMENT_NEW(mTaskPool.allocate(sizeof(SolveIslandTask)), SolveIslandTask)(islandContext, islandContext.mObjects, islandContext.mCounts,  mDt, mThreadContext, *this);

	FinishSolveIslandTask* finishTask = PX_PLACEMENT_NEW(mTaskPool.allocate(sizeof(FinishSolveIslandTask)), FinishSolveIslandTask)(mThreadContext, islandContext.mObjects, islandContext.mCounts, islandManager, *this);

	EndIslandTask* endTask = PX_PLACEMENT_NEW(mTaskPool.allocate(sizeof(EndIslandTask)), EndIslandTask)(mThreadContext, *this);

	endTask->setContinuation(continuation);
	finishTask->setContinuation(endTask);
	solveTask->setContinuation(finishTask);
	constraintTask->setContinuation(solveTask);
	partitionTask->setContinuation(constraintTask);
	articConTask->setContinuation(partitionTask);
	//Stepper triggers both articCon and constraintTask
	stepperTask->setContinuation(articConTask);
	stepperTask->setAdditionalContinuation(constraintTask);

	articTask->setContinuation(stepperTask);
	intTask->setContinuation(stepperTask);
	descTask->setContinuation(stepperTask);

	endTask->removeReference();
	finishTask->removeReference();
	solveTask->removeReference();
	constraintTask->removeReference();
	partitionTask->removeReference();
	articConTask->removeReference();
	stepperTask->removeReference();
	articTask->removeReference();
	intTask->removeReference();
	descTask->removeReference();	
}

void DynamicsTGSContext::mergeResults()
{
	PX_PROFILE_ZONE("Dynamics.solverMergeResults", mContextID);
	//OK. Sum up sim stats here...

#if PX_ENABLE_SIM_STATS	
	{
		PxcThreadCoherentCacheIterator<ThreadContext, PxcNpMemBlockPool> threadContextIt(mThreadContextPool);
		ThreadContext* threadContext = threadContextIt.getNext();

		mTotalContactError.reset();
		mContactErrorPosIter = &mTotalContactError.mPositionIterationErrorAccumulator;
		mContactErrorVelIter = &mTotalContactError.mVelocityIterationErrorAccumulator;

		while (threadContext != NULL)
		{
			if (mIsResidualReportingEnabled)
			{
				Dy::ErrorAccumulatorEx& error = threadContext->getSimStats().contactErrorAccumulator;
				mTotalContactError.combine(error);
				threadContext->getSimStats().contactErrorAccumulator.reset();
			}

			threadContext = threadContextIt.getNext();
		}
	}
#else
	PX_CATCH_UNDEFINED_ENABLE_SIM_STATS
#endif
}

}
}

