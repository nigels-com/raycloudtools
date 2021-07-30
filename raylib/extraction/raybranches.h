// Copyright (c) 2021
// Commonwealth Scientific and Industrial Research Organisation (CSIRO)
// ABN 41 687 119 230
//
// Author: Thomas Lowe
#ifndef RAYLIB_RAYBRANCHES_H
#define RAYLIB_RAYBRANCHES_H

#include "raylib/raylibconfig.h"
#include "../rayutils.h"
#include "../raycloud.h"


namespace ray
{
#define LENGTH

struct Bush
{
  Bush(const Cloud &cloud, double midRadius, bool verbose);
  bool save(const std::string &filename);
  static std::vector<std::pair<Eigen::Vector3d, double> > load(const std::string &filename);
  std::vector<Trunk> trunk_bases;
};


} // namespace ray
#endif // RAYLIB_RAYBRANCHES_H