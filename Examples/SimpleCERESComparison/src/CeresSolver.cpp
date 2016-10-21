#pragma once

#include <cuda_runtime.h>

#include "config.h"

#include "CeresSolver.h"

#include "ceres/ceres.h"
#include "glog/logging.h"

using ceres::DynamicAutoDiffCostFunction;
using ceres::AutoDiffCostFunction;
using ceres::CostFunction;
using ceres::Problem;
using ceres::Solver;
using ceres::Solve;
using namespace std;

struct TermDefault
{
	TermDefault(double x, double y)
        : x(x), y(y) {}

    template <typename T>
    bool operator()(const T* const funcParams, T* residuals) const
    {
        
        residuals[0] = y - (funcParams[0] * cos(funcParams[1] * x) + funcParams[1] * sin(funcParams[0]*x));
        return true;
    }

    static ceres::CostFunction* Create(double x, double y)
    {
		return (new ceres::AutoDiffCostFunction<TermDefault, 1, 2>(
			new TermDefault(x, y)));
    }

    double x;
    double y;
};

struct HackRegularizerTerm
{
    HackRegularizerTerm(double weight) : m_weight(weight) {}

    template <typename T>
    bool operator()(const T* const funcParams, T* residuals) const
    {

        residuals[0] = (funcParams[0] - funcParams[1]) * m_weight;
        return true;
    }

    static ceres::CostFunction* Create(double weight)
    {
        return (new ceres::AutoDiffCostFunction<HackRegularizerTerm, 1, 2>(
            new HackRegularizerTerm(weight)));
    }
    double m_weight;
};

struct TermMirsa
{
	TermMirsa(double x, double y)
		: x(x), y(y) {}

	template <typename T>
	bool operator()(const T* const funcParams, T* residuals) const
	{
		residuals[0] = y - funcParams[0] * ((T)1.0 - exp(-funcParams[1] * x));
		return true;
	}

	static ceres::CostFunction* Create(double x, double y)
	{
		return (new ceres::AutoDiffCostFunction<TermMirsa, 1, 2>(
			new TermMirsa(x, y)));
	}

	double x;
	double y;
};

struct TermBennet5
{
	TermBennet5(double x, double y)
		: x(x), y(y) {}

	template <typename T>
	bool operator()(const T* const funcParams, T* residuals) const
	{
		residuals[0] = y - funcParams[0] * pow(funcParams[1] + x, -1.0 / funcParams[2]);
		return true;
	}

	static ceres::CostFunction* Create(double x, double y)
	{
		return (new ceres::AutoDiffCostFunction<TermBennet5, 1, 3>(
			new TermBennet5(x, y)));
	}

	double x;
	double y;
};

struct TermChwirut1
{
	TermChwirut1(double x, double y)
		: x(x), y(y) {}

	template <typename T>
	bool operator()(const T* const funcParams, T* residuals) const
	{
		residuals[0] = y - exp(-funcParams[0] * x) / (funcParams[1] + funcParams[2] * x);
		return true;
	}

	static ceres::CostFunction* Create(double x, double y)
	{
		return (new ceres::AutoDiffCostFunction<TermChwirut1, 1, 3>(
			new TermChwirut1(x, y)));
	}

	double x;
	double y;
};

void CeresSolver::solve(
	const NLLSProblem &problemInfo,
    UNKNOWNS* funcParameters,
    double2* funcData)
{
    for (int i = 0; i < functionData.size(); i++)
    {
        functionData[i].x = funcData[i].x;
        functionData[i].y = funcData[i].y;

    }

    Problem problem;
    for (int i = 0; i < functionData.size(); i++)
    {
		ceres::CostFunction* costFunction = nullptr;

		if (useProblemDefault) costFunction = TermDefault::Create(functionData[i].x, functionData[i].y);
		if (problemInfo.baseName == "misra") costFunction = TermMirsa::Create(functionData[i].x, functionData[i].y);
		if (problemInfo.baseName == "bennet5") costFunction = TermBennet5::Create(functionData[i].x, functionData[i].y);
		if (problemInfo.baseName == "chwirut1") costFunction = TermChwirut1::Create(functionData[i].x, functionData[i].y);

		if (costFunction == nullptr)
		{
			cout << "No problem specified!" << endl;
			cin.get();
		}
		problem.AddResidualBlock(costFunction, NULL, (double*)funcParameters);
    }
    /*
    ceres::CostFunction* regCostFunction = HackRegularizerTerm::Create(10000.0);
    problem.AddResidualBlock(regCostFunction, NULL, (double*)funcParameters);
    */
    cout << "Solving..." << endl;

    Solver::Options options;
    Solver::Summary summary;

    options.minimizer_progress_to_stdout = true;

    //faster methods
    options.num_threads = 1;
    options.num_linear_solver_threads = 1;
    //options.linear_solver_type = ceres::LinearSolverType::SPARSE_NORMAL_CHOLESKY; //7.2s
    //options.linear_solver_type = ceres::LinearSolverType::SPARSE_SCHUR; //10.0s

    //slower methods
    //options.linear_solver_type = ceres::LinearSolverType::ITERATIVE_SCHUR; //40.6s
    options.linear_solver_type = ceres::LinearSolverType::CGNR; //46.9s

    //options.min_linear_solver_iterations = linearIterationMin;
    options.max_num_iterations = 10000;
    options.function_tolerance = 1e-20;
    options.gradient_tolerance = 1e-10 * options.function_tolerance;

    // Default values, reproduced here for clarity
    options.trust_region_strategy_type = ceres::TrustRegionStrategyType::LEVENBERG_MARQUARDT;
    options.initial_trust_region_radius = 1e4;
    options.max_trust_region_radius = 1e16;
    options.min_trust_region_radius = 1e-32;
    options.min_relative_decrease = 1e-3;
    // Disable to match Opt
    options.min_lm_diagonal = 1e-32;
    options.max_lm_diagonal = std::numeric_limits<double>::infinity();


    options.initial_trust_region_radius = 1e4;

    options.jacobi_scaling = true;

    Solve(options, &problem, &summary);


    cout << "Solver used: " << summary.linear_solver_type_used << endl;
    cout << "Minimizer iters: " << summary.iterations.size() << endl;

    double iterationTotalTime = 0.0;
    int totalLinearItereations = 0;
    for (auto &i : summary.iterations)
    {
        iterationTotalTime += i.iteration_time_in_seconds;
        totalLinearItereations += i.linear_solver_iterations;
        //cout << "Iteration: " << i.linear_solver_iterations << " " << i.iteration_time_in_seconds * 1000.0 << "ms" << endl;
    }

    cout << "Total iteration time: " << iterationTotalTime << endl;
    cout << "Cost per linear solver iteration: " << iterationTotalTime * 1000.0 / totalLinearItereations << "ms" << endl;

    double cost = -1.0;
    problem.Evaluate(Problem::EvaluateOptions(), &cost, nullptr, nullptr, nullptr);
    cout << "Cost*2 end: " << cost * 2 << endl;

    cout << summary.FullReport() << endl;
}

