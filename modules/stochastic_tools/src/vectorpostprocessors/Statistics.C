//* This file is part of the MOOSE framework
//* https://www.mooseframework.org
//*
//* All rights reserved, see COPYRIGHT for full restrictions
//* https://github.com/idaholab/moose/blob/master/COPYRIGHT
//*
//* Licensed under LGPL 2.1, please see LICENSE for details
//* https://www.gnu.org/licenses/lgpl-2.1.html

#include "Statistics.h"

// MOOSE includes
#include "MooseVariable.h"
#include "ThreadedElementLoopBase.h"
#include "ThreadedNodeLoop.h"

#include "libmesh/quadrature.h"

#include <numeric>

registerMooseObject("MooseApp", Statistics);

defineLegacyParams(Statistics);

InputParameters
Statistics::validParams()
{
  InputParameters params = GeneralVectorPostprocessor::validParams();
  params.addClassDescription(
      "Compute statistical values of a given VectorPostprocessor objects and vectors.");

  params.addRequiredParam<std::vector<VectorPostprocessorName>>(
      "vectorpostprocessors",
      "List of VectorPostprocessor(s) to utilized for statistic computations.");

  MultiMooseEnum stats = StochasticTools::makeCalculatorEnum();
  params.addRequiredParam<MultiMooseEnum>(
      "compute",
      stats,
      "The statistic(s) to compute for each of the supplied vector postprocessors.");

  // Confidence Levels
  MooseEnum ci = StochasticTools::makeBootstrapCalculatorEnum();
  params.addParam<MooseEnum>(
      "ci_method", ci, "The method to use for computing confidence level intervals.");

  params.addParam<std::vector<Real>>(
      "ci_levels", "A vector of confidence levels to consider, values must be in (0, 0.5].");
  params.addParam<unsigned int>(
      "ci_replicates",
      10000,
      "The number of replicates to use when computing confidence level intervals.");
  params.addParam<unsigned int>("ci_seed",
                                1,
                                "The random number generator seed used for creating replicates "
                                "while computing confidence level intervals.");

  return params;
}

Statistics::Statistics(const InputParameters & parameters)
  : GeneralVectorPostprocessor(parameters),
    _compute_stats(getParam<MultiMooseEnum>("compute")),
    _ci_method(getParam<MooseEnum>("ci_method")),
    _ci_levels(_ci_method.isValid() ? computeLevels(getParam<std::vector<Real>>("ci_levels"))
                                    : std::vector<Real>()),
    _stat_type_vector(declareVector("stat_type"))
{
  for (const auto & item : _compute_stats)
  {
    _stat_type_vector.push_back(item.id());
    for (const auto & level : _ci_levels)
      _stat_type_vector.push_back(item.id() + level);
  }

  if (_ci_method.isValid())
  {
    unsigned int replicates = getParam<unsigned int>("ci_replicates");
    unsigned int seed = getParam<unsigned int>("ci_seed");
    _ci_calculator =
        StochasticTools::makeBootstrapCalculator(_ci_method, *this, _ci_levels, replicates, seed);
  }
}

void
Statistics::initialSetup()
{
  const auto & vpp_names = getParam<std::vector<VectorPostprocessorName>>("vectorpostprocessors");
  for (const auto & vpp_name : vpp_names)
  {
    const std::vector<std::pair<std::string, VectorPostprocessorData::VectorPostprocessorState>> &
        vpp_vectors = _fe_problem.getVectorPostprocessorVectors(vpp_name);
    for (const auto & the_pair : vpp_vectors)
    {
      // Store VectorPostprocessor name and vector name from which stats will be computed
      _compute_from_names.emplace_back(vpp_name, the_pair.first, the_pair.second.is_distributed);

      // Create the vector where the statistics will be stored
      std::string name = vpp_name + "_" + the_pair.first;
      _stat_vectors.push_back(&declareVector(name));
    }
  }
}

void
Statistics::execute()
{
  for (std::size_t i = 0; i < _compute_from_names.size(); ++i)
  {
    const std::string & vpp_name = std::get<0>(_compute_from_names[i]);
    const std::string & vec_name = std::get<1>(_compute_from_names[i]);
    const bool is_distributed = std::get<2>(_compute_from_names[i]);
    const VectorPostprocessorValue & data =
        _fe_problem.getVectorPostprocessorValue(vpp_name, vec_name, true);

    if (is_distributed || processor_id() == 0)
    {
      for (const auto & item : _compute_stats)
      {
        std::unique_ptr<const StochasticTools::Calculator> calc_ptr =
            StochasticTools::makeCalculator(item, *this);
        _stat_vectors[i]->emplace_back(calc_ptr->compute(data, is_distributed));

        if (_ci_calculator)
        {
          std::vector<Real> ci = _ci_calculator->compute(data, *calc_ptr, is_distributed);
          _stat_vectors[i]->insert(_stat_vectors[i]->end(), ci.begin(), ci.end());
        }
      }
    }
  }
}

std::vector<Real>
Statistics::computeLevels(const std::vector<Real> & levels_in) const
{
  if (levels_in.empty())
    paramError("ci_levels",
               "If the 'ci_method' parameter is supplied then the 'ci_levels' must also be "
               "supplied with values in (0, 0.5].");

  else if (*std::min_element(levels_in.begin(), levels_in.end()) <= 0)
    paramError("ci_levels", "The supplied levels must be greater than zero.");

  else if (*std::max_element(levels_in.begin(), levels_in.end()) > 0.5)
    paramError("ci_levels", "The supplied levels must be less than or equal to 0.5");

  std::list<Real> levels_out;
  for (auto it = levels_in.rbegin(); it != levels_in.rend(); ++it)
  {
    if (*it == 0.5)
      levels_out.push_back(*it);

    else
    {
      levels_out.push_front(*it);
      levels_out.push_back(1 - *it);
    }
  }
  return std::vector<Real>(levels_out.begin(), levels_out.end());
}
