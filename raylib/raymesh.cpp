// Copyright (c) 2020
// Commonwealth Scientific and Industrial Research Organisation (CSIRO)
// ABN 41 687 119 230
//
// Author: Thomas Lowe
#include "raymesh.h"

#include "raylaz.h"
#include "rayply.h"
#include "rayunused.h"

#include <set>

namespace ray
{
class Triangle
{
public:
  Eigen::Vector3d corners[3];
  Eigen::Vector3d normal;
  bool tested;
  bool intersectsRay(const Eigen::Vector3d &ray_start, const Eigen::Vector3d &ray_end, double &depth)
  {
    // 1. plane test:
    double d1 = (ray_start - corners[0]).dot(normal);
    double d2 = (ray_end - corners[0]).dot(normal);
    if (d1 * d2 > 0.0)
      return false;

    depth = d1 / (d1 - d2);
    Eigen::Vector3d contact_point = ray_start + (ray_end - ray_start) * depth;

    // next we have to test every sideways direction
    for (int i = 0; i < 3; i++)
    {
      Eigen::Vector3d side = (corners[(i + 1) % 3] - corners[i]).cross(normal);
      if ((contact_point - corners[i]).dot(side) >= 0.0)
        return false;
    }
    return true;
  }
  double distSqrToPoint(const Eigen::Vector3d &point)
  {
    Eigen::Vector3d pos = point - normal * (point - corners[0]).dot(normal);
    bool outs[3];
    double ds[3];
    Eigen::Vector3d sides[3];
    for (int i = 0; i < 3; i++)
    {
      sides[i] = (corners[(i + 1) % 3] - corners[i]).cross(normal);
      ds[i] = (pos - corners[i]).dot(sides[i]);
      outs[i] = ds[i] > 0.0;
    }
    if (outs[0] && outs[1])
      pos = corners[1];
    else if (outs[1] && outs[2])
      pos = corners[2];
    else if (outs[2] && outs[0])
      pos = corners[0];
    else if (outs[0])
      pos -= sides[0] * ds[0] / sides[0].squaredNorm();
    else if (outs[1])
      pos -= sides[1] * ds[1] / sides[1].squaredNorm();
    else if (outs[2])
      pos -= sides[2] * ds[2] / sides[2].squaredNorm();
    return (point - pos).squaredNorm();
  }
  bool intersectsCube(const Eigen::Vector3d &cube_min, double cube_width)
  {
    RAYLIB_UNUSED(cube_min);
    RAYLIB_UNUSED(cube_width);
    // TODO: fill in
    return true;
  }
};

// remove additional points that are not connected to the mesh
void Mesh::reduce()
{
  std::vector<Eigen::Vector3d> verts;
  std::vector<int> new_ids(vertices_.size(), -1);
  // we do this by iterating the index list, and only adding the vertices that are in these triangles
  for (auto &ind : index_list_)
  {
    for (int i = 0; i < 3; i++)
    {
      if (new_ids[ind[i]] == -1)
      {
        new_ids[ind[i]] = (int)verts.size();
        verts.push_back(vertices_[ind[i]]);
      }
      ind[i] = new_ids[ind[i]];
    }
  }
  vertices_ = verts;
}

// convert the mesh to a height field
void Mesh::toHeightField(Eigen::ArrayXXd &field, const Eigen::Vector3d &box_min, Eigen::Vector3d box_max,
                         double width) const
{
  double top = box_max[2];
  box_max[2] = box_min[2] + 0.5 * width;  // ensure that the grid is only 1 voxel high
  // first convert the mesh to a list of triangles, with calculated normals
  std::vector<Triangle> triangles(index_list_.size());
  for (int i = 0; i < (int)index_list_.size(); i++)
  {
    Triangle &tri = triangles[i];
    for (int j = 0; j < 3; j++) 
    {
      tri.corners[j] = vertices_[index_list_[i][j]];
    }
    tri.tested = false;
    tri.normal = (tri.corners[1] - tri.corners[0]).cross(tri.corners[2] - tri.corners[0]);
  }

  // put the triangles into a grid
  Grid<Triangle *> grid(box_min, box_max, width);
  for (auto &tri : triangles)
  {
    Eigen::Vector3d tri_min = (minVector(tri.corners[0], minVector(tri.corners[1], tri.corners[2])) - box_min) / width;
    Eigen::Vector3d tri_max = (maxVector(tri.corners[0], maxVector(tri.corners[1], tri.corners[2])) - box_min) / width;
    for (int x = (int)tri_min[0]; x <= (int)tri_max[0]; x++)
    {
      for (int y = (int)tri_min[1]; y <= (int)tri_max[1]; y++) 
      {
        grid.insert(x, y, 0, &tri);
      }
    }
  }
  // now look up the triangle for each pixel centre
  const double unset = std::numeric_limits<double>::lowest();
  field = Eigen::ArrayXXd::Constant(grid.dims[0], grid.dims[1], unset);
  std::cout << "dims for low: " << grid.dims.transpose() << ", rows: " << field.rows() << ", cols: " << field.cols()
            << std::endl;
  for (int x = 0; x < grid.dims[0]; x++)
  {
    for (int y = 0; y < grid.dims[1]; y++)
    {
      Eigen::Vector3d pos_top = box_min + width * (Eigen::Vector3d((double)x + 0.5, (double)y + 0.5, 0));
      Eigen::Vector3d pos_base = pos_top;
      pos_top[2] = top;
      pos_base[2] = box_min[2];
      auto &tris = grid.cell(x, y, 0).data;
      // search the triangles in this cell 'bucket'
      for (auto &tri : tris)
      {
        double depth;
        if (tri->intersectsRay(pos_top, pos_base, depth))
        {
          // intersects so interpolate the height
          double height = pos_top[2] + (pos_base[2] - pos_top[2]) * depth;
          field(x, y) = height;
          break;
        }
      }
    }
  }
  // lastly, we repeatedly fill in the gaps
  bool gaps_remain = true;
  while (gaps_remain)
  {
    gaps_remain = false;
    for (int x = 0; x < grid.dims[0]; x++)
    {
      for (int y = 0; y < grid.dims[1]; y++)
      {
        if (field(x, y) == unset)
        {
          double count = 0;
          double total_height = 0;
          // look at the Moore neighbourhood to obtain a mean neighbour height
          for (int i = std::max(0, x - 1); i <= std::min(x + 1, grid.dims[0] - 1); i++)
          {
            for (int j = std::max(0, y - 1); j <= std::min(y + 1, grid.dims[1] - 1); j++)
            {
              if (field(i, j) != unset)
              {
                total_height += field(i, j);
                count++;
              }
            }
          }
          // Note that this immediate modifier is not order/direction independant,
          // but it doesn't matter too much, as there should be very few gaps anyway
          if (count > 0)
            field(x, y) = total_height / count;
          else
            gaps_remain = true;
        }
      }
    }
  }
}

void Mesh::splitCloud(const Cloud &cloud, double offset, Cloud &inside, Cloud &outside)
{
  // Firstly, find the average vertex normals
  std::vector<Eigen::Vector3d> normals(vertices_.size());
  for (auto &normal : normals) normal.setZero();
  for (auto &index : index_list_)
  {
    Eigen::Vector3d normal =
      (vertices_[index[1]] - vertices_[index[0]]).cross(vertices_[index[2]] - vertices_[index[0]]);
    for (int i = 0; i < 3; i++) normals[index[i]] += normal;
  }
  for (auto &normal : normals) normal.normalize();

  // convert to separate triangles for convenience
  std::vector<Triangle> triangles(index_list_.size());
  double mx = std::numeric_limits<double>::max();
  double mn = std::numeric_limits<double>::lowest();
  Eigen::Vector3d box_min(mx, mx, mx), box_max(mn, mn, mn);
  for (int i = 0; i < (int)index_list_.size(); i++)
  {
    Triangle &tri = triangles[i];
    for (int j = 0; j < 3; j++) tri.corners[j] = vertices_[index_list_[i][j]];
    tri.tested = false;
    tri.normal = (tri.corners[1] - tri.corners[0]).cross(tri.corners[2] - tri.corners[0]).normalized();
    for (int j = 0; j < 3; j++)
    {
      box_min = minVector(box_min, tri.corners[j]);
      box_max = maxVector(box_max, tri.corners[j]);
    }
  }

  // Thirdly, put the triangles into a grid
  double voxel_width = 1.0;
  std::vector<int> inside_indices;
  {
    int inside_val = offset >= 0.0 ? 1 : 0;
    Grid<Triangle *> grid(box_min, box_max, voxel_width);
    for (auto &tri : triangles)
    {
      Eigen::Vector3d tri_min =
        (minVector(tri.corners[0], minVector(tri.corners[1], tri.corners[2])) - box_min) / voxel_width;
      Eigen::Vector3d tri_max =
        (maxVector(tri.corners[0], maxVector(tri.corners[1], tri.corners[2])) - box_min) / voxel_width;
      for (int x = (int)tri_min[0]; x <= (int)tri_max[0]; x++)
      {
        for (int y = (int)tri_min[1]; y <= (int)tri_max[1]; y++)
        {
          for (int z = (int)tri_min[2]; z <= (int)tri_max[2]; z++)
          {
            grid.insert(x, y, z, &tri);
          }
        }
      }
    }

    // Fourthly, drop each end point downwards to decide whether it is inside or outside..
    std::vector<Triangle *> tris_tested;
    tris_tested.reserve(100.0);
    for (int r = 0; r < (int)cloud.ends.size(); r++)
    {
      int intersections = 0;
      // for (int dir = -1; dir<=1; dir += 2)
      int dir = -1;
      {
        Eigen::Vector3d start = (cloud.ends[r] - box_min) / voxel_width;
        Eigen::Vector3i index(start.cast<int>());
        int end_i = dir < 0 ? 0 : grid.dims[2] - 1;
        tris_tested.clear();
        for (int z = clamped(index[2], 0, grid.dims[2] - 1); (z * dir) <= end_i; z += dir)
        {
          auto &tris = grid.cell(index[0], index[1], z).data;
          for (auto &tri : tris)
          {
            if (tri->tested)
              continue;
            tri->tested = true;
            tris_tested.push_back(tri);
            double depth;
            if (tri->intersectsRay(cloud.ends[r], cloud.ends[r] + (double)dir * Eigen::Vector3d(0.0, 0.0, 1e3), depth))
              intersections++;
          }
        }
        for (auto &tri : tris_tested) tri->tested = false;
      }
      if ((intersections % 2) == inside_val)  // inside
        inside_indices.push_back(r);
    }
  }
  std::cout << inside_indices.size() << "/" << cloud.ends.size() << " inside mesh" << std::endl;

  // what if offset is negative?
  // we have to ...
  if (offset != 0.0)
  {
    // Thirdly, put the triangles into a grid
    double voxel_width = 1.0;
    Grid<Triangle *> grid2(box_min, box_max, voxel_width);
    for (int i = 0; i < (int)index_list_.size(); i++)
    {
      if (!(i % 100000))
        std::cout << "filling volumes " << i << "/" << index_list_.size() << std::endl;
      Triangle &tri = triangles[i];
      Eigen::Vector3d extruded_corners[3];
      for (int j = 0; j < 3; j++) extruded_corners[j] = tri.corners[j] + normals[index_list_[i][j]] * offset;

      Eigen::Vector3d tri_min = minVector(tri.corners[0], minVector(tri.corners[1], tri.corners[2]));
      Eigen::Vector3d tri_max = maxVector(tri.corners[0], maxVector(tri.corners[1], tri.corners[2]));
      Eigen::Vector3d tri_min2 = minVector(extruded_corners[0], minVector(extruded_corners[1], extruded_corners[2]));
      Eigen::Vector3d tri_max2 = maxVector(extruded_corners[0], maxVector(extruded_corners[1], extruded_corners[2]));

      tri_min = (minVector(tri_min, tri_min2) - box_min) / voxel_width;
      tri_max = (maxVector(tri_max, tri_max2) - box_min) / voxel_width;
      for (int x = (int)tri_min[0]; x <= (int)tri_max[0]; x++)
        for (int y = (int)tri_min[1]; y <= (int)tri_max[1]; y++)
          for (int z = (int)tri_min[2]; z <= (int)tri_max[2]; z++) grid2.insert(x, y, z, &tri);
    }
    // now go through the remaining inside points
    std::vector<int> new_insides;
    double offset_sqr = sqr(offset);
    int p = 0;
    for (auto &r : inside_indices)
    {
      if (!(p++ % 1000000))
        std::cout << "checking points " << p - 1 << "/" << inside_indices.size() << std::endl;
      Eigen::Vector3d pos = (cloud.ends[r] - box_min) / voxel_width;
      Eigen::Vector3i index(pos.cast<int>());
      auto &tris = grid2.cell(index[0], index[1], index[2]).data;
      bool in_tri = false;
      for (auto &tri : tris)
      {
        if (tri->distSqrToPoint(cloud.ends[r]) < offset_sqr)
        {
          in_tri = true;
          break;
        };
      }
      if (!in_tri)
        new_insides.push_back(r);
    }
    std::cout << "new inside count: " << new_insides.size() << "/" << cloud.ends.size() << std::endl;
    inside_indices = new_insides;
  }

  std::vector<bool> inside_i(cloud.ends.size());
  int ins = offset >= 0.0;
  for (int i = 0; i < (int)cloud.ends.size(); i++) inside_i[i] = !ins;
  for (auto &ind : inside_indices) inside_i[ind] = ins;
  for (int i = 0; i < (int)cloud.ends.size(); i++)
  {
    Cloud &out = inside_i[i] ? inside : outside;
    out.addRay(cloud, i);
  }
}

Eigen::Array<double, 6, 1> Mesh::getMoments() const
{
  Eigen::Array3d mean(0, 0, 0);
  for (auto &v : vertices_) 
  {
    mean += v.array();
  }
  mean /= (double)vertices_.size();
  Eigen::Array3d sigma(0, 0, 0);
  for (auto &v : vertices_) 
  {
    sigma += (v.array() - mean) * (v.array() - mean);
  }
  sigma = (sigma / (double)vertices_.size()).sqrt();
  Eigen::Array<double, 6, 1> result;
  result << mean, sigma;
  return result;
}

}  // namespace ray
