#pragma once

#include <cuda_runtime.h>
#include <cuda_d3d11_interop.h>

#include "../../cudaUtil.h"
#include "PatchSolverWarpingState.h"

class CUDAPatchSolverWarping
{
	public:

		CUDAPatchSolverWarping(unsigned int imageWidth, unsigned int imageHeight);
		~CUDAPatchSolverWarping();

		void solveGN(float* d_image, float* d_target, unsigned int nNonLinearIterations, unsigned int nPatchIterations, float weightFitting, float weightRegularizer);
			
	private:

		PatchSolverState m_solverState;

		unsigned int m_imageWidth;
		unsigned int m_imageHeight;
};