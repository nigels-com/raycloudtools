// Copyright (c) 2020
// Commonwealth Scientific and Industrial Research Organisation (CSIRO)
// ABN 41 687 119 230
//
// Author: Thomas Lowe
#include "raycloud.h"

#include "raydebugdraw.h"
#include "raylaz.h"
#include "rayply.h"
#include "rayprogress.h"

#include <nabo/nabo.h>

#include <iostream>
#include <limits>
#include <set>
// #define OUTPUT_CLOUD_MOMENTS // useful for setting up unit tests comparisons

namespace ray
{

void Cloud::clear()
{
  starts.clear();
  ends.clear();
  times.clear();
  colours.clear();
}

void Cloud::save(const std::string &file_name) const
{
  std::string name = file_name;
  if (name.substr(name.length() - 4) != ".ply")
    name += ".ply";
  writePlyRayCloud(name, starts, ends, times, colours);
  #if defined OUTPUT_CLOUD_MOMENTS
  getMoments();
  #endif // defined OUTPUT_CLOUD_MOMENTS
}

bool Cloud::load(const std::string &file_name)
{
  // look first for the raycloud PLY
  if (file_name.substr(file_name.size() - 4) == ".ply")
    return loadPLY(file_name);

  return false;
}

bool Cloud::loadPLY(const std::string &file)
{
  return readPly(file, starts, ends, times, colours, true);
}

Eigen::Vector3d Cloud::calcMinBound() const
{
  Eigen::Vector3d min_v(std::numeric_limits<double>::max(), std::numeric_limits<double>::max(), std::numeric_limits<double>::max());
  for (int i = 0; i < (int)ends.size(); i++)
  {
    if (rayBounded(i))
      min_v = minVector(min_v, minVector(starts[i], ends[i]));
  }
  return min_v;
}

Eigen::Vector3d Cloud::calcMaxBound() const
{
  Eigen::Vector3d max_v(std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest());
  for (int i = 0; i < (int)ends.size(); i++)
  {
    if (rayBounded(i))
      max_v = maxVector(max_v, maxVector(starts[i], ends[i]));
  }
  return max_v;
}

Eigen::Vector3d Cloud::calcMinPointBound() const
{
  Eigen::Vector3d min_v(std::numeric_limits<double>::max(), std::numeric_limits<double>::max(), std::numeric_limits<double>::max());
  for (int i = 0; i < (int)ends.size(); i++)
  {
    if (rayBounded(i))
      min_v = minVector(min_v, ends[i]);
  }
  return min_v;
}

Eigen::Vector3d Cloud::calcMaxPointBound() const
{
  Eigen::Vector3d max_v(std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest());
  for (int i = 0; i < (int)ends.size(); i++)
  {
    if (rayBounded(i))
      max_v = maxVector(max_v, ends[i]);
  }
  return max_v;
}

bool Cloud::calcBounds(Eigen::Vector3d *min_bounds, Eigen::Vector3d *max_bounds, unsigned flags, Progress *progress) const
{
  if (rayCount() == 0)
  {
    return false;
  }

  if (progress)
  {
    progress->begin("calcBounds", rayCount());
  }

  *min_bounds = Eigen::Vector3d(std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
                         std::numeric_limits<double>::max());
  *max_bounds = Eigen::Vector3d(std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest(),
                         std::numeric_limits<double>::lowest());
  bool invalid_bounds = true;
  for (size_t i = 0; i < rayCount(); ++i)
  {
    if (rayBounded(i))
    {
      invalid_bounds = false;
      if (flags & kBFEnd)
      {
        *min_bounds = minVector(*min_bounds, ends[i]);
        *max_bounds = maxVector(*max_bounds, ends[i]);
      }
      if (flags & kBFStart)
      {
        *min_bounds = minVector(*min_bounds, starts[i]);
        *max_bounds = maxVector(*max_bounds, starts[i]);
      }
    }

    if (progress)
    {
      progress->increment();
    }
  }

  return !invalid_bounds;
}

void Cloud::transform(const Pose &pose, double time_delta)
{
  for (int i = 0; i < (int)starts.size(); i++)
  {
    starts[i] = pose * starts[i];
    ends[i] = pose * ends[i];
    times[i] += time_delta;
  }
}

void Cloud::removeUnboundedRays()
{
  std::vector<int> valids;
  for (int i = 0; i < (int)ends.size(); i++)
    if (rayBounded(i))
      valids.push_back(i);
  for (int i = 0; i < (int)valids.size(); i++)
  {
    starts[i] = starts[valids[i]];
    ends[i] = ends[valids[i]];
    times[i] = times[valids[i]];
    colours[i] = colours[valids[i]];
  }
  starts.resize(valids.size());
  ends.resize(valids.size());
  times.resize(valids.size());
  colours.resize(valids.size());
}

void Cloud::decimate(double voxel_width, std::set<Eigen::Vector3i, Vector3iLess> &voxel_set)
{
  std::vector<int64_t> subsample;
  voxelSubsample(ends, voxel_width, subsample, voxel_set);
  for (int64_t i = 0; i < (int64_t)subsample.size(); i++)
  {
    const int64_t id = subsample[i];
    starts[i] = starts[id];
    ends[i] = ends[id];
    colours[i] = colours[id];
    times[i] = times[id];
  }
  starts.resize(subsample.size());
  ends.resize(subsample.size());
  colours.resize(subsample.size());
  times.resize(subsample.size());
}

void Cloud::getSurfels(int search_size, std::vector<Eigen::Vector3d> *centroids, std::vector<Eigen::Vector3d> *normals,
                       std::vector<Eigen::Vector3d> *dimensions, std::vector<Eigen::Matrix3d> *mats, 
                       Eigen::MatrixXi *neighbour_indices)
{
  // simplest scheme... find 3 nearest neighbours and do cross product
  if (centroids)
    centroids->resize(ends.size());
  if (normals)
    normals->resize(ends.size());
  if (dimensions)
    dimensions->resize(ends.size());
  if (mats)
    mats->resize(ends.size());
  Nabo::NNSearchD *nns;
  std::vector<int> ray_ids;
  ray_ids.reserve(ends.size());
  for (unsigned int i = 0; i < ends.size(); i++)
    if (rayBounded(i))
      ray_ids.push_back(i);
  Eigen::MatrixXd points_p(3, ray_ids.size());
  for (unsigned int i = 0; i < ray_ids.size(); i++) 
    points_p.col(i) = ends[ray_ids[i]];
  nns = Nabo::NNSearchD::createKDTreeLinearHeap(points_p, 3);

  // Run the search
  Eigen::MatrixXi indices;
  Eigen::MatrixXd dists2;
  indices.resize(search_size, ray_ids.size());
  dists2.resize(search_size, ray_ids.size());
  nns->knn(points_p, indices, dists2, search_size, kNearestNeighbourEpsilon, 0);
  delete nns;

  if (neighbour_indices)
    neighbour_indices->resize(search_size, ends.size());
  for (int i = 0; i < (int)ray_ids.size(); i++)
  {
    int ray_id = ray_ids[i];
    if (neighbour_indices)
    {
      int j;
      for (j = 0; j < search_size && indices(j, i) > -1; j++) 
        (*neighbour_indices)(j, ray_id) = ray_ids[indices(j, i)];
      if (j < search_size)
        (*neighbour_indices)(j, ray_id) = -1;
    }

    Eigen::Vector3d centroid = ends[ray_id];
    int num;
    for (num = 0; num < search_size && indices(num, i) > -1; num++) centroid += ends[ray_ids[indices(num, i)]];
    centroid /= (double)(num + 1);
    if (centroids)
      (*centroids)[ray_id] = centroid;

    Eigen::Matrix3d scatter = (ends[ray_id] - centroid) * (ends[ray_id] - centroid).transpose();
    for (int j = 0; j < num; j++)
    {
      Eigen::Vector3d offset = ends[ray_ids[indices(j, i)]] - centroid;
      scatter += offset * offset.transpose();
    }
    scatter /= (double)(num + 1);

    Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> eigen_solver(scatter.transpose());
    ASSERT(eigen_solver.info() == Eigen::ComputationInfo::Success);
    if (normals)
    {
      Eigen::Vector3d normal = eigen_solver.eigenvectors().col(0);
      if ((ends[ray_id] - starts[ray_id]).dot(normal) > 0.0)
        normal = -normal;
      (*normals)[ray_id] = normal;
    }
    if (dimensions)
    {
      Eigen::Vector3d eigenvals = maxVector(Eigen::Vector3d(1e-10, 1e-10, 1e-10), eigen_solver.eigenvalues());
      (*dimensions)[ray_id] = Eigen::Vector3d(std::sqrt(eigenvals[0]), std::sqrt(eigenvals[1]), std::sqrt(eigenvals[2]));
    }
    if (mats)
      (*mats)[ray_id] = eigen_solver.eigenvectors();
  }
}

// starts are required to get the normal the right way around
std::vector<Eigen::Vector3d> Cloud::generateNormals(int search_size)
{
  std::vector<Eigen::Vector3d> normals;
  getSurfels(search_size, NULL, &normals, NULL, NULL, NULL);
  return normals;
}

bool RAYLIB_EXPORT Cloud::getInfo(const std::string &file_name, Cuboid &ends, Cuboid &starts, Cuboid &rays,
                                  int &num_bounded, int &num_unbounded)
{
  double min_s = std::numeric_limits<double>::max();
  double max_s = std::numeric_limits<double>::lowest();
  Eigen::Vector3d min_v(min_s, min_s, min_s);
  Eigen::Vector3d max_v(max_s, max_s, max_s);
  Cuboid unbounded(min_v, max_v);
  ends = starts = rays = unbounded;
  num_unbounded = num_bounded = 0;
  auto find_bounds = [&](std::vector<Eigen::Vector3d> &start_list, std::vector<Eigen::Vector3d> &end_list, std::vector<double> &, std::vector<ray::RGBA> &colours)
  {
    for (size_t i = 0; i<end_list.size(); i++)
    {
      if (colours[i].alpha > 0)
      {
        ends.min_bound_ = minVector(ends.min_bound_, end_list[i]);
        ends.max_bound_ = maxVector(ends.max_bound_, end_list[i]);
        num_bounded++;
      }
      num_unbounded++;
      starts.min_bound_ = minVector(starts.min_bound_, start_list[i]);
      starts.max_bound_ = maxVector(starts.max_bound_, start_list[i]);
      rays.min_bound_ = minVector(rays.min_bound_, end_list[i]);
      rays.max_bound_ = maxVector(rays.max_bound_, end_list[i]);
    }
    rays.min_bound_ = minVector(rays.min_bound_, starts.min_bound_);
    rays.max_bound_ = maxVector(rays.max_bound_, starts.max_bound_);
  };  
  return readPly(file_name, true, find_bounds, 0);
}


double Cloud::estimatePointSpacing(std::string &file_name, const Cuboid &bounds, int num_points)
{
  // two-iteration estimation, modelling the point distribution by the below exponent.
  // larger exponents (towards 2.5) match thick forests, lower exponents (towards 2) match smooth terrain and surfaces
  const double cloud_exponent = 2.0; // model num_points = (cloud_width/voxel_width)^cloud_exponent

  Eigen::Vector3d extent = bounds.max_bound_ - bounds.min_bound_;
  double cloud_width = pow(extent[0]*extent[1]*extent[2], 1.0/3.0); // an average
  double voxel_width = cloud_width / pow((double)num_points, 1.0/cloud_exponent);
  voxel_width *= 5.0; // we want to use a larger width because this process only works when the width is an overestimation
  std::cout << "initial voxel width estimate: " << voxel_width << std::endl;
  double num_voxels = 0;
  std::set<Eigen::Vector3i, Vector3iLess> test_set;

  auto estimate_size = [&](std::vector<Eigen::Vector3d> &, std::vector<Eigen::Vector3d> &ends, std::vector<double> &, std::vector<ray::RGBA> &colours)
  {
    for (unsigned int i = 0; i < ends.size(); i++)
    {
      if (colours[i].alpha == 0)
        continue;

      const Eigen::Vector3d &point = ends[i];
      Eigen::Vector3i place(int(std::floor(point[0] / voxel_width)), int(std::floor(point[1] / voxel_width)),
                            int(std::floor(point[2] / voxel_width)));
      if (test_set.find(place) == test_set.end())
      {
        test_set.insert(place);
        num_voxels++;
      }
    }
  };  
  if (!readPly(file_name, true, estimate_size, 0))
    return 0;

  double points_per_voxel = (double)num_points / num_voxels;
  double width = voxel_width / pow(points_per_voxel, 1.0/cloud_exponent);
  std::cout << "estimated point spacing: " << width << std::endl;
  return width;
}

double Cloud::estimatePointSpacing() const
{
  // two-iteration estimation, modelling the point distribution by the below exponent.
  // larger exponents (towards 2.5) match thick forests, lower exponents (towards 2) match smooth terrain and surfaces
  const double cloud_exponent = 2.0; // model num_points = (cloud_width/voxel_width)^cloud_exponent

  Eigen::Vector3d min_bound, max_bound;
  calcBounds(&min_bound, &max_bound, kBFEnd);
  Eigen::Vector3d extent = max_bound - min_bound;
  int num_points = 0;
  for (unsigned int i = 0; i < ends.size(); i++)
    if (rayBounded(i))
      num_points++;
  double cloud_width = pow(extent[0]*extent[1]*extent[2], 1.0/3.0); // an average
  double voxel_width = cloud_width / pow((double)num_points, 1.0/cloud_exponent);
  voxel_width *= 5.0; // we want to use a larger width because this process only works when the width is an overestimation
  std::cout << "initial voxel width estimate: " << voxel_width << std::endl;
  double num_voxels = 0;
  std::set<Eigen::Vector3i, Vector3iLess> test_set;
  for (unsigned int i = 0; i < ends.size(); i++)
  {
    if (rayBounded(i))
    {
      const Eigen::Vector3d &point = ends[i];
      Eigen::Vector3i place(int(std::floor(point[0] / voxel_width)), int(std::floor(point[1] / voxel_width)),
                            int(std::floor(point[2] / voxel_width)));
      if (test_set.find(place) == test_set.end())
      {
        test_set.insert(place);
        num_voxels++;
      }
    }
  }
  double points_per_voxel = (double)num_points / num_voxels;
  double width = voxel_width / pow(points_per_voxel, 1.0/cloud_exponent);
  std::cout << "estimated point spacing: " << width << std::endl;
  return width;
}

void Cloud::split(Cloud &cloud1, Cloud &cloud2, std::function<bool(int i)> fptr)
{
  for (int i = 0; i < (int)ends.size(); i++)
  {
    Cloud &cloud = fptr(i) ? cloud2 : cloud1;
    cloud.addRay(*this, i);
  }
}

void Cloud::addRay(const Eigen::Vector3d &start, const Eigen::Vector3d &end, double time, const RGBA &colour)
{
  starts.push_back(start);
  ends.push_back(end);
  times.push_back(time);
  colours.push_back(colour);
}


void Cloud::addRay(const Cloud &other_cloud, size_t index)
{
  starts.push_back(other_cloud.starts[index]);
  ends.push_back(other_cloud.ends[index]);
  times.push_back(other_cloud.times[index]);
  colours.push_back(other_cloud.colours[index]);
}

void Cloud::resize(size_t size)
{
  starts.resize(size);
  ends.resize(size);
  times.resize(size);
  colours.resize(size);
}

Eigen::Array<double, 22, 1> Cloud::getMoments() const
{
  Eigen::Vector3d startMean(0,0,0);
  Eigen::Array3d startSigma(0,0,0);
  Eigen::Vector3d endMean(0,0,0);
  Eigen::Array3d endSigma(0,0,0);
  double timeMean = 0.0;
  double timeSigma = 0.0;
  Eigen::Vector4d colourMean(0,0,0,0);
  Eigen::Array4d colourSigma(0,0,0,0);
  for (size_t i = 0; i<ends.size(); i++)
  {
    startMean += starts[i];
    endMean += ends[i];
    timeMean += times[i];
    colourMean += Eigen::Vector4d(colours[i].red, colours[i].green, colours[i].blue, colours[i].alpha) / 255.0;
  }  
  startMean /= (double)ends.size();
  endMean /= (double)ends.size();
  timeMean /= (double)ends.size();
  colourMean /= (double)ends.size();
  for (size_t i = 0; i<ends.size(); i++)
  {
    Eigen::Array3d start = (starts[i] - startMean).array();
    startSigma += start * start;
    Eigen::Array3d end = (ends[i] - endMean).array();
    endSigma += end * end;
    timeSigma += ray::sqr(times[i] - timeMean);
    Eigen::Vector4d colour(colours[i].red, colours[i].green, colours[i].blue, colours[i].alpha);
    Eigen::Array4d col = (colour / 255.0 - colourMean).array();
    colourSigma += col * col;
  }   
  startSigma = (startSigma / (double)ends.size()).sqrt();
  endSigma = (endSigma / (double)ends.size()).sqrt();
  timeSigma = std::sqrt(timeSigma / (double)ends.size());
  colourSigma = (colourSigma / (double)ends.size()).sqrt();  

  Eigen::Array<double, 22, 1> result;
  result << startMean, startSigma, endMean, endSigma, timeMean, timeSigma, colourMean, colourSigma;
  std::cout << "stats: ";
  for (int i = 0; i<22; i++)
    std::cout << ", " << result[i];
  std::cout << std::endl;
  return result; // Note: this is used once per cloud, returning by value is not a performance issue
}


} // namespace ray
