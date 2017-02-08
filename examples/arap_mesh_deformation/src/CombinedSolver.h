#pragma once

#include "mLibInclude.h"

#include <cuda_runtime.h>
#include <cudaUtil.h>
#include "Configure.h"
#include "TerraWarpingSolver.h"
#include "CUDAWarpingSolver.h"
#include "OpenMesh.h"
#include "CeresWarpingSolver.h"
#include "../../shared/SolverIteration.h"
#include "../../shared/Precision.h"
#include "../../shared/CombinedSolverParameters.h"
#include "../../shared/CombinedSolverBase.h"
#include <cuda_profiler_api.h>
// From the future (C++14)
template<typename T, typename... Args>
std::unique_ptr<T> make_unique(Args&&... args) {
    return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
}


class CombinedSolver : public CombinedSolverBase
{
	public:
        CombinedSolver(const SimpleMesh* mesh, std::vector<int> constraintsIdx, std::vector<std::vector<float>> constraintsTarget, CombinedSolverParameters params) : m_constraintsIdx(constraintsIdx), m_constraintsTarget(constraintsTarget)
		{
			m_result = *mesh;
			m_initial = m_result;


			unsigned int N = (unsigned int)mesh->n_vertices();
			unsigned int E = (unsigned int)mesh->n_edges();

			cutilSafeCall(cudaMalloc(&d_vertexPosTargetFloat3, sizeof(float3)*N));

			cutilSafeCall(cudaMalloc(&d_vertexPosFloat3, sizeof(float3)*N));
			cutilSafeCall(cudaMalloc(&d_vertexPosFloat3Urshape, sizeof(float3)*N));
			cutilSafeCall(cudaMalloc(&d_anglesFloat3, sizeof(float3)*N));
			cutilSafeCall(cudaMalloc(&d_numNeighbours, sizeof(int)*N));
			cutilSafeCall(cudaMalloc(&d_neighbourIdx, sizeof(int)*2*E));
			cutilSafeCall(cudaMalloc(&d_neighbourOffset, sizeof(int)*(N+1)));
		
			resetGPUMemory();
			

			m_warpingSolver	= new CUDAWarpingSolver(N);
            m_optWarpingSolver = make_unique<TerraWarpingSolver>(N, 2 * E, d_neighbourIdx, d_neighbourOffset, "arap_mesh_deformation.t", "gaussNewtonGPU");
            m_optLMWarpingSolver = make_unique<TerraWarpingSolver>(N, 2 * E, d_neighbourIdx, d_neighbourOffset, "arap_mesh_deformation.t", "LMGPU");
            m_ceresWarpingSolver = new CeresWarpingSolver(m_initial);
		} 

        virtual void combinedSolveInit() override {
            float weightFit = 1.0f;
            float weightReg = 0.05f;

            m_weightFitSqrt = sqrtf(weightFit);
            m_weightRegSqrt = sqrtf(weightReg);

            m_problemParams.set("Offset", m_gridPosFloat3);
            m_problemParams.set("Angle", m_gridAnglesFloat3);
            m_problemParams.set("UrShape", m_gridPosFloat3Urshape);
            m_problemParams.set("Constraints", m_gridPosTargetFloat3);
            m_problemParams.set("w_fitSqrt", &m_weightFitSqrt);
            m_problemParams.set("w_regSqrt", &m_weightRegSqrt);


            m_solverParams.set("nonLinearIterations", &m_combinedSolverParameters.nonLinearIter);
            m_solverParams.set("linearIterations", &m_combinedSolverParameters.linearIter);
            m_solverParams.set("double_precision", &m_combinedSolverParameters.optDoublePrecision);
        }
        virtual void preSingleSolve() override {
            m_result = m_initial;
            resetGPUMemory();
        }
        virtual void postSingleSolve() override {
            copyResultToCPUFromFloat3();
        }
        virtual void combinedSolveFinalize() override {
            if (m_combinedSolverParameters.profileSolve) {
                ceresIterationComparison();
            }
        }

		void setConstraints(float alpha)
		{
			unsigned int N = (unsigned int)m_result.n_vertices();
			float3* h_vertexPosTargetFloat3 = new float3[N];
			for (unsigned int i = 0; i < N; i++)
			{
				h_vertexPosTargetFloat3[i] = make_float3(-std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity());
			}

			for (unsigned int i = 0; i < m_constraintsIdx.size(); i++)
			{
				const Vec3f& pt = m_result.point(VertexHandle(m_constraintsIdx[i]));
				const Vec3f target = Vec3f(m_constraintsTarget[i][0], m_constraintsTarget[i][1], m_constraintsTarget[i][2]);

				Vec3f z = (1 - alpha)*pt + alpha*target;
				h_vertexPosTargetFloat3[m_constraintsIdx[i]] = make_float3(z[0], z[1], z[2]);
			}
			cutilSafeCall(cudaMemcpy(d_vertexPosTargetFloat3, h_vertexPosTargetFloat3, sizeof(float3)*N, cudaMemcpyHostToDevice));
			delete [] h_vertexPosTargetFloat3;
		}

		void resetGPUMemory()
		{
			unsigned int N = (unsigned int)m_initial.n_vertices();
			unsigned int E = (unsigned int)m_initial.n_edges();

			float3* h_vertexPosFloat3 = new float3[N];
			int*	h_numNeighbours   = new int[N];
			int*	h_neighbourIdx	  = new int[2*E];
			int*	h_neighbourOffset = new int[N+1];

			for (unsigned int i = 0; i < N; i++)
			{
				const Vec3f& pt = m_initial.point(VertexHandle(i));
				h_vertexPosFloat3[i] = make_float3(pt[0], pt[1], pt[2]);
			}

			unsigned int count = 0;
			unsigned int offset = 0;
			h_neighbourOffset[0] = 0;
			for (SimpleMesh::VertexIter v_it = m_initial.vertices_begin(); v_it != m_initial.vertices_end(); ++v_it)
			{
			    VertexHandle c_vh(v_it.handle());
				unsigned int valance = m_initial.valence(c_vh);
				h_numNeighbours[count] = valance;

				for (SimpleMesh::VertexVertexIter vv_it = m_initial.vv_iter(c_vh); vv_it; vv_it++)
				{
					VertexHandle v_vh(vv_it.handle());

					h_neighbourIdx[offset] = v_vh.idx();
					offset++;
				}

				h_neighbourOffset[count + 1] = offset;

				count++;
			}
			
			// Constraints
			setConstraints(1.0f);


			// Angles
			float3* h_angles = new float3[N];
			for (unsigned int i = 0; i < N; i++)
			{
				h_angles[i] = make_float3(0.0f, 0.0f, 0.0f);
			}
			cutilSafeCall(cudaMemcpy(d_anglesFloat3, h_angles, sizeof(float3)*N, cudaMemcpyHostToDevice));
			delete [] h_angles;
			
			cutilSafeCall(cudaMemcpy(d_vertexPosFloat3, h_vertexPosFloat3, sizeof(float3)*N, cudaMemcpyHostToDevice));
			cutilSafeCall(cudaMemcpy(d_vertexPosFloat3Urshape, h_vertexPosFloat3, sizeof(float3)*N, cudaMemcpyHostToDevice));
			cutilSafeCall(cudaMemcpy(d_numNeighbours, h_numNeighbours, sizeof(int)*N, cudaMemcpyHostToDevice));
			cutilSafeCall(cudaMemcpy(d_neighbourIdx, h_neighbourIdx, sizeof(int)* 2 * E, cudaMemcpyHostToDevice));
			cutilSafeCall(cudaMemcpy(d_neighbourOffset, h_neighbourOffset, sizeof(int)*(N + 1), cudaMemcpyHostToDevice));

			delete [] h_vertexPosFloat3;
			delete [] h_numNeighbours;
			delete [] h_neighbourIdx;
			delete [] h_neighbourOffset;
		}

        ~CombinedSolver()
		{
			cutilSafeCall(cudaFree(d_anglesFloat3));

			cutilSafeCall(cudaFree(d_vertexPosTargetFloat3));
			cutilSafeCall(cudaFree(d_vertexPosFloat3));
			cutilSafeCall(cudaFree(d_vertexPosFloat3Urshape));
			cutilSafeCall(cudaFree(d_numNeighbours));
			cutilSafeCall(cudaFree(d_neighbourIdx));
			cutilSafeCall(cudaFree(d_neighbourOffset));

			SAFE_DELETE(m_warpingSolver);
		}

		SimpleMesh* solve()
		{
			
			//float weightFit = 6.0f;
            //float weightReg = 0.5f;
			float weightFit = 3.0f;
			float weightReg = 4.0f; //0.000001f;

            auto gpuSolve = [&](std::string name, bool condition, std::function<void(void)> solveFunc) {
                if (condition) {
                    m_result = m_initial;
                    resetGPUMemory();
                    for (unsigned int i = 1; i < m_params.numIter; i++)
                    {
                        std::cout << "//////////// ITERATION" << i << "  (" << name << ") ///////////////" << std::endl;
                        setConstraints((float)i / (float)(m_params.numIter - 1));

                        solveFunc();
                        if (m_params.earlyOut) {
                            break;
                        }
                    }
                    copyResultToCPUFromFloat3();
                }
            };

            auto cudaSolve = [=](){m_warpingSolver->solveGN(d_vertexPosFloat3, d_anglesFloat3, d_vertexPosFloat3Urshape, d_numNeighbours, d_neighbourIdx, d_neighbourOffset, d_vertexPosTargetFloat3, m_params.nonLinearIter, m_params.linearIter, weightFit, weightReg); };
            gpuSolve("CUDA", m_params.useCUDA != 0, cudaSolve);

            auto genericOptSolve = [=](std::unique_ptr<TerraWarpingSolver>& solver, std::vector<SolverIteration>& iters) { solver->solveGN(d_vertexPosFloat3, d_anglesFloat3, d_vertexPosFloat3Urshape, d_vertexPosTargetFloat3, m_params.nonLinearIter, m_params.linearIter, weightFit, weightReg, iters); };

            gpuSolve("TERRA", m_params.useTerra != 0, [=](){ genericOptSolve(m_terraWarpingSolver, m_terraIters); });
            gpuSolve("OPT", m_params.useOpt != 0, [=](){ genericOptSolve(m_optWarpingSolver, m_optIters); });
            gpuSolve("OPT_LM", m_params.useOptLM != 0, [=](){ genericOptSolve(m_optLMWarpingSolver, m_optLMIters); });


            if (m_params.useCeres) {
                m_result = m_initial;
                resetGPUMemory();

                unsigned int N = (unsigned int)m_initial.n_vertices();
                unsigned int E = (unsigned int)m_initial.n_edges();

                float3* h_vertexPosFloat3 = new float3[N];
                float3* h_vertexPosFloat3Urshape = new float3[N];
                int*	h_numNeighbours = new int[N];
                int*	h_neighbourIdx = new int[2 * E];
                int*	h_neighbourOffset = new int[N + 1];
                float3* h_anglesFloat3 = new float3[N];
                float3* h_vertexPosTargetFloat3 = new float3[N];

                cutilSafeCall(cudaMemcpy(h_anglesFloat3, d_anglesFloat3, sizeof(float3)*N, cudaMemcpyDeviceToHost));
                cutilSafeCall(cudaMemcpy(h_vertexPosFloat3, d_vertexPosFloat3, sizeof(float3)*N, cudaMemcpyDeviceToHost));
                cutilSafeCall(cudaMemcpy(h_vertexPosFloat3Urshape, d_vertexPosFloat3Urshape, sizeof(float3)*N, cudaMemcpyDeviceToHost));
                cutilSafeCall(cudaMemcpy(h_numNeighbours, d_numNeighbours, sizeof(int)*N, cudaMemcpyDeviceToHost));
                cutilSafeCall(cudaMemcpy(h_neighbourIdx, d_neighbourIdx, sizeof(int) * 2 * E, cudaMemcpyDeviceToHost));
                cutilSafeCall(cudaMemcpy(h_neighbourOffset, d_neighbourOffset, sizeof(int)*(N + 1), cudaMemcpyDeviceToHost));

                float finalIterTime;
                for (unsigned int i = 1; i < m_params.numIter; i++)
                {
                    std::cout << "//////////// ITERATION" << i << "  (CERES) ///////////////" << std::endl;
                    setConstraints((float)i / (float)(m_params.numIter - 1));
                    cutilSafeCall(cudaMemcpy(h_vertexPosTargetFloat3, d_vertexPosTargetFloat3, sizeof(float3)*N, cudaMemcpyDeviceToHost));

                    finalIterTime = m_ceresWarpingSolver->solveGN(h_vertexPosFloat3, h_anglesFloat3, h_vertexPosFloat3Urshape, h_vertexPosTargetFloat3, weightFit, weightReg, m_ceresIters);
                    if (m_params.earlyOut) {
                        break;
                    }

                }
                std::cout << "CERES final iter time: " << finalIterTime << "ms" << std::endl;

                cutilSafeCall(cudaMemcpy(d_vertexPosFloat3, h_vertexPosFloat3, sizeof(float3)*N, cudaMemcpyHostToDevice));
                copyResultToCPUFromFloat3();
            }
			cudaDeviceSynchronize();
			cudaProfilerStop();

			std::string suffix = OPT_DOUBLE_PRECISION ? "_double" : "_float";
			if (m_lmOnlyFullSolve) {
				unsigned int N = (unsigned int)m_initial.n_vertices();
				suffix += std::to_string(N);
			}

			saveSolverResults("results/", suffix, m_ceresIters, m_optIters, m_optLMIters);
			
            //reportFinalCosts("Mesh Deformation ARAP", m_params, m_optWarpingSolver->finalCost(), m_optLMWarpingSolver->finalCost(), m_ceresWarpingSolver->finalCost());
            cudaProfilerStop();
			return &m_result;
		}

		void copyResultToCPUFromFloat3()
		{
			unsigned int N = (unsigned int)m_result.n_vertices();
			float3* h_vertexPosFloat3 = new float3[N];
			cutilSafeCall(cudaMemcpy(h_vertexPosFloat3, d_vertexPosFloat3, sizeof(float3)*N, cudaMemcpyDeviceToHost));

			for (unsigned int i = 0; i < N; i++)
			{
				m_result.set_point(VertexHandle(i), Vec3f(h_vertexPosFloat3[i].x, h_vertexPosFloat3[i].y, h_vertexPosFloat3[i].z));
			}

			delete [] h_vertexPosFloat3;
		}

	private:

		SimpleMesh m_result;
		SimpleMesh m_initial;
	
		float3* d_anglesFloat3;
		float3*	d_vertexPosTargetFloat3;
		float3*	d_vertexPosFloat3;
		float3*	d_vertexPosFloat3Urshape;
		int*	d_numNeighbours;
		int*	d_neighbourIdx;
		int* 	d_neighbourOffset;

        std::vector<SolverIteration> m_optIters;
        std::vector<SolverIteration> m_optLMIters;
        std::vector<SolverIteration> m_terraIters;
        std::vector<SolverIteration> m_ceresIters;

		std::unique_ptr<TerraWarpingSolver> m_optWarpingSolver;
        std::unique_ptr<TerraWarpingSolver> m_optLMWarpingSolver;
		std::unique_ptr<TerraWarpingSolver> m_terraWarpingSolver;
        CeresWarpingSolver* m_ceresWarpingSolver;
		CUDAWarpingSolver*	m_warpingSolver;

		std::vector<int>				m_constraintsIdx;
		std::vector<std::vector<float>>	m_constraintsTarget;
};