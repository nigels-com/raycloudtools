// Copyright (c) 2020
// Commonwealth Scientific and Industrial Research Organisation (CSIRO)
// ABN 41 687 119 230
//
// Author: Thomas Lowe
#include "rayrenderer.h"
#include "raycloud.h"
#include "rayply.h"
#include "rayparse.h"
#include "imagewrite.h"

#define DENSITY_MIN_RAYS 10 // larger is more accurate but more blurred. 0 for no adaptive blending

namespace ray
{
  
// density is the probability of hitting something per metre depth
void DensityGrid::calculateDensities(const std::string &file_name)
{
  auto calculate = [&](std::vector<Eigen::Vector3d> &starts, std::vector<Eigen::Vector3d> &ends, std::vector<double> &, std::vector<RGBA> &colours)
  {
    for (size_t i = 0; i<ends.size(); i++)
    {
      Eigen::Vector3d start = starts[i];
      Eigen::Vector3d end   = ends[i];
      bounds_.clipRay(start, end);

      // now walk the voxels
      Eigen::Vector3d dir = end - start;
      Eigen::Vector3d source = (start - bounds_.min_bound_)/voxel_width_;
      Eigen::Vector3d p = source;
      Eigen::Vector3d target = (end - bounds_.min_bound_)/voxel_width_;
      double length = dir.norm();
      double maxDist = (target - source).norm();
      Eigen::Vector3i inds = p.cast<int>();
      double depth = 0;
      do
      {
        int axis = 0;
        double minL = 1e10;
        for (int k = 0; k<3; k++)
        {
          double l = (dir[k] > 0 ? ceil(p[k]) - p[k] : p[k] - floor(p[k])) * length / abs(dir[k]);
          if (l < minL)
          {
            minL = l;
            axis = k;
          }
        }
        depth += minL + 1e-9;
        inds[axis] += dir[axis] > 0 ? 1 : -1;
        if (inds[axis] < 0 || inds[axis] >= voxel_dims_[axis])
          break;
        p = source + depth * dir / length;
        int j = getIndex(inds);
        if (colours[i].alpha > 0 && depth > maxDist)
        {
          double d = minL + maxDist - depth;
          voxels_[j].addHitRay(static_cast<float>(d*voxel_width_));
        }
        else
        {
          voxels_[j].addMissRay(static_cast<float>(minL*voxel_width_)); 
        }
      } while (depth <= maxDist);
    }
  };
  readPly(file_name, true, calculate, 0);
}

// This is a form of windowed average over the Moore neighbourhood (3x3x3) window.
void DensityGrid::addNeighbourPriors()
{
  int X = 1;
  int Y = voxel_dims_[0];
  int Z = voxel_dims_[0]*voxel_dims_[1];
  DensityGrid::Voxel neighbours;
  double num_hit_points = 0.0;
  double num_hit_points_unsatisfied = 0.0;

  // This simple 3x3x3 convolution needs to be a bit sneaky to avoid having to double the memory cost.
  // well, not that sneaky, we just shift the output -1,-1,-1 for each cell
  for (int x = 1; x<voxel_dims_[0]-1; x++)
  {
    for (int y = 1; y<voxel_dims_[1]-1; y++)
    {
      for (int z = 1; z<voxel_dims_[2]-1; z++)
      {
        int ind = getIndex(Eigen::Vector3i(x,y,z));
        if (voxels_[ind].numHits() > 0)
          num_hit_points++;
        float needed = DENSITY_MIN_RAYS - voxels_[ind].numRays();
        DensityGrid::Voxel corner_vox = voxels_[ind - X - Y - Z];
        voxels_[ind - X - Y - Z] = voxels_[ind]; // move centre up to corner 
        DensityGrid::Voxel &voxel = voxels_[ind - X - Y - Z]; 
        if (needed < 0.0)
          continue;
        neighbours  = voxels_[ind-X];
        neighbours += voxels_[ind+X];
        neighbours += voxels_[ind-Y];
        neighbours += voxels_[ind+Y];
        neighbours += voxels_[ind-Z];
        neighbours += voxels_[ind+Z];
        if (neighbours.numRays() >= needed)
        {
          voxel += neighbours * (needed/neighbours.numRays()); // add minimal amount to reach DENSITY_MIN_RAYS
          continue;
        }
        voxel += neighbours;
        needed -= neighbours.numRays();

        neighbours  = voxels_[ind-X-Y];
        neighbours += voxels_[ind-X+Y];
        neighbours += voxels_[ind+X-Y];
        neighbours += voxels_[ind+X+Y];

        neighbours += voxels_[ind-X-Z];
        neighbours += voxels_[ind-X+Z];
        neighbours += voxels_[ind+X-Z];
        neighbours += voxels_[ind+X+Z];

        neighbours += voxels_[ind-Y-Z];
        neighbours += voxels_[ind-Y+Z];
        neighbours += voxels_[ind+Y-Z];
        neighbours += voxels_[ind+Y+Z];
        if (neighbours.numRays() >= needed)
        {
          voxel += neighbours * (needed/neighbours.numRays()); // add minimal amount to reach DENSITY_MIN_RAYS
          continue;
        }
        voxel += neighbours;
        needed -= neighbours.numRays();

        neighbours  = corner_vox;          
        neighbours += voxels_[ind-X-Y+Z];          
        neighbours += voxels_[ind-X+Y-Z];          
        neighbours += voxels_[ind+X-Y-Z];          
        neighbours += voxels_[ind-X+Y+Z];          
        neighbours += voxels_[ind+X-Y+Z];          
        neighbours += voxels_[ind+X+Y-Z];          
        neighbours += voxels_[ind+X+Y+Z];     
        if (neighbours.numRays() >= needed)
        {
          voxel += neighbours * (needed/neighbours.numRays()); // add minimal amount to reach DENSITY_MIN_RAYS
          continue;
        }
        voxel += neighbours;    
        if (voxels_[ind].numHits() > 0)
          num_hit_points_unsatisfied++;
      }
    }
  }
  double percentage = 100.0*num_hit_points_unsatisfied/num_hit_points;
  std::cout << "Density calculation: " << percentage << "% of voxels had insufficient (<" 
    << DENSITY_MIN_RAYS << ") rays within them" << std::endl;
  if (percentage > 50.0)
  {
    std::cout << "This is high. Consider using a larger pixel size, or a denser cloud, or reducing DENSITY_MIN_RAYS, for consistent results"
      << std::endl;
  }
  else if (percentage < 1.0)
  {
    std::cout << "This is low enough that you could get more fidelity from using a smaller pixel size" << std::endl;
    std::cout << "or more accuracy by increasing DENSITY_MIN_RAYS" << std::endl;
  }
}

bool renderCloud(const std::string &cloud_file, const Cuboid &bounds, ViewDirection view_direction, 
                 RenderStyle style, double pix_width, const std::string &image_file)                 
{
  // convert the view direction into useable parameters
  int axis = 0;
  if (view_direction == ViewDirection::Top)
    axis = 2;
  else if (view_direction == ViewDirection::Front || view_direction == ViewDirection::Back)
    axis = 1;
  double dir = 1;
  if (view_direction == ViewDirection::Left || view_direction == ViewDirection::Front)
    dir = -1;
  bool flip_x = view_direction == ViewDirection::Left || view_direction == ViewDirection::Back;
  
  // pull out the main image axes (ax1,ax2 are the horiz,vertical axes)
  Eigen::Vector3d extent = bounds.max_bound_ - bounds.min_bound_;
  int x_axes[] = {1, 0, 0};
  int y_axes[] = {2, 2, 1};
  int ax1 = x_axes[axis];
  int ax2 = y_axes[axis];
  int width  = 1 + static_cast<int>(extent[ax1] / pix_width);
  int height = 1 + static_cast<int>(extent[ax2] / pix_width);
  int depth  = 1 + static_cast<int>(extent[axis] / pix_width);
  std::cout << "outputting " << width << "x" << height << " image" << std::endl;

  // accumulated colour buffer
  std::vector<Eigen::Vector4d> pixels(width * height); 
  memset(&pixels[0], 0, sizeof(Eigen::Vector4d) * width*height);
  // density calculation is a special case
  if (style == RenderStyle::Density || style == RenderStyle::Density_rgb) 
  {
    Eigen::Vector3i dims = (extent/pix_width).cast<int>() + Eigen::Vector3i(1,1,1);
    #if DENSITY_MIN_RAYS > 0
    dims += Eigen::Vector3i(1,1,1); // so that we have extra space to convolve
    #endif
    Cuboid grid_bounds = bounds;
    grid_bounds.min_bound_ -= Eigen::Vector3d(pix_width, pix_width, pix_width);
    DensityGrid grid(grid_bounds, pix_width, dims);

    grid.calculateDensities(cloud_file);

    #if DENSITY_MIN_RAYS > 0
    grid.addNeighbourPriors();
    #endif

    for (int x = 0; x < width; x++)
    {
      for (int y = 0; y < height; y++)
      {
        double total_density = 0.0;
        for (int z = 0; z< depth; z++)
        {
          Eigen::Vector3i ind;
          ind[axis] = z;
          ind[ax1] = x;
          ind[ax2] = y;
          total_density += grid.voxels()[grid.getIndex(ind)].density();
        }
        pixels[x + width * y] = Eigen::Vector4d(total_density, total_density, total_density, total_density);
      }
    }
  }
  else // otherwise we use a common algorithm, specialising on render style only per-ray
  {
    // this lambda expression lets us chunk load the ray cloud file, so we don't run out of RAM
    auto render = [&](std::vector<Eigen::Vector3d> &starts, std::vector<Eigen::Vector3d> &ends, std::vector<double> &, std::vector<RGBA> &colours)
    {
      for (size_t i = 0; i<ends.size(); i++)
      {
        RGBA &colour = colours[i];
        if (colour.alpha == 0)
          continue;
        Eigen::Vector3d col = Eigen::Vector3d(colour.red, colour.green, colour.blue)/255.0;
        Eigen::Vector3d point = style == RenderStyle::Starts ? starts[i] : ends[i];
        Eigen::Vector3d pos = (point - bounds.min_bound_) / pix_width;
        Eigen::Vector3i p = (pos).cast<int>();
        int x = p[ax1], y = p[ax2];
        // using 4 dimensions helps us to accumulate colours in a greater variety of ways
        Eigen::Vector4d &pix = pixels[x + width*y]; 
        switch (style)
        {
          case RenderStyle::Ends: 
          case RenderStyle::Starts: 
            // TODO: fix the == 0.0 part in future, it can cause incorrect occlusion on points with z=0 precisely
            if (pos[axis]*dir > pix[3]*dir || pix[3] == 0.0) 
              pix = Eigen::Vector4d(col[0], col[1], col[2], pos[axis]);
            break;
          case RenderStyle::Mean: 
            pix += Eigen::Vector4d(col[0], col[1], col[2], 1.0);
            break;
          case RenderStyle::Sum: 
            pix += Eigen::Vector4d(col[0], col[1], col[2], 1.0);
            break;
          case RenderStyle::Rays: 
          {
            Eigen::Vector3d cloud_start = starts[i];
            Eigen::Vector3d cloud_end = ends[i];
            // clip to within the image (since we exclude unbounded rays from the image bounds)
            bounds.clipRay(cloud_start, cloud_end); 
            Eigen::Vector3d start = (cloud_start - bounds.min_bound_) / pix_width;
            Eigen::Vector3d end = (cloud_end - bounds.min_bound_) / pix_width;
            Eigen::Vector3d dir = cloud_end - cloud_start;

            // fast approximate 2D line rendering requires picking the long axis to iterate along
            bool x_long = std::abs(dir[ax1]) > std::abs(dir[ax2]);
            int axis_long   = x_long ? ax1 : ax2;
            int axis_short  = x_long ? ax2 : ax1;
            int width_long  = x_long ? 1 : width;
            int width_short = x_long ? width : 1;

            double gradient = dir[axis_short] / dir[axis_long]; 
            if (dir[axis_long] < 0.0)
              std::swap(start, end); // this lets us iterate from low up to high values
            int start_long = static_cast<int>(start[axis_long]);
            int end_long = static_cast<int>(end[axis_long]);
            // place a pixel at the height of each midpoint (of the pixel) in the long axis
            double start_mid_point = 0.5 + static_cast<double>(start_long);
            double height = start[axis_short] + (start_mid_point - start[axis_long])*gradient;
            for (int l = start_long; l <= end_long; l++, height += gradient)
            {
              int s = static_cast<int>(height);
              pixels[width_long*l + width_short*s] += Eigen::Vector4d(col[0], col[1], col[2], 1.0);
            }
            break;
          }
          default:
            break;
        }
      }
    };
    if (!readPly(cloud_file, true, render, 0))
      return false;
  }

  double max_val = 1.0;
  std::string image_ext = getFileNameExtension(image_file);
  bool is_hdr = image_ext == "hdr";
  if (!is_hdr) // limited range, so work out a sensible maximum value, I'm using mean + two standard deviations:
  {
    double sum = 0.0;
    double num = 0.0;
    for (auto &pixel: pixels)
    {
      sum += pixel[3];
      if (pixel[3] > 0.0)
        num++;
    }
    double mean = sum / num;
    double sum_sqr = 0.0;
    for (auto &pixel: pixels)
    {
      if (pixel[3] > 0.0)
        sum_sqr += sqr(pixel[3] - mean);
    }
    double standard_deviation = std::sqrt(sum_sqr / num);
    max_val = mean + 2.0*standard_deviation;
  }

  // The final pixel buffer
  std::vector<RGBA> pixel_colours;
  std::vector<float> float_pixel_colours;
  if (is_hdr)
    float_pixel_colours.resize(3 * width * height);
  else
    pixel_colours.resize(width*height);

  for (int x = 0; x < width; x++)
  {
    int indx = flip_x ? width - 1 - x : x; // possible horizontal flip, depending on view direction
    for (int y = 0; y < height; y++)
    {
      Eigen::Vector4d colour = pixels[x + width*y];
      Eigen::Vector3d col3d(colour[0], colour[1], colour[2]);
      uint8_t alpha = colour[3] == 0.0 ? 0 : 255; // 'punch-through' alpha
      switch (style)
      {
        case RenderStyle::Mean:
        case RenderStyle::Rays: 
          col3d /= colour[3]; // simple mean
          break;
        case RenderStyle::Sum: 
        case RenderStyle::Density: 
          col3d /= max_val; // rescale to within limited colour range
          break;
        case RenderStyle::Density_rgb: 
        {
          if (is_hdr)
            col3d = colour[0] * redGreenBlueSpectrum(std::log10(std::max(1e-6, colour[0])));
          else 
          {
            double shade = colour[0] / max_val;
            col3d = redGreenBlueGradient(shade);
            if (shade < 0.05)
              col3d *= 20.0*shade; // this blends the lowest densities down to black
          }
          break;
        }
        default:
          break;
      }
      int ind = indx + width *y;
      if (is_hdr)
      {
        float_pixel_colours[3*ind + 0] = (float)col3d[0];
        float_pixel_colours[3*ind + 1] = (float)col3d[1];
        float_pixel_colours[3*ind + 2] = (float)col3d[2];
      }
      else 
      {
        RGBA col;
        col.red   = uint8_t(std::min(255.0*col3d[0], 255.0));
        col.green = uint8_t(std::min(255.0*col3d[1], 255.0));
        col.blue  = uint8_t(std::min(255.0*col3d[2], 255.0));
        col.alpha = alpha;
        pixel_colours[ind] = col;
      }
    }
  }
  std::cout << "outputting image: " << image_file << std::endl;
  const char *image_name = image_file.c_str();
  stbi_flip_vertically_on_write(1);
  if (image_ext == "png")
    stbi_write_png(image_name, width, height, 4, (void *)&pixel_colours[0], 4 * width);
  else if (image_ext == "bmp")
    stbi_write_bmp(image_name, width, height, 4, (void *)&pixel_colours[0]);
  else if (image_ext == "tga")
    stbi_write_tga(image_name, width, height, 4, (void *)&pixel_colours[0]);
  else if (image_ext == "png")
    stbi_write_jpg(image_name, width, height, 4, (void *)&pixel_colours[0], 100); // maximal quality
  else if (image_ext == "hdr")
    stbi_write_hdr(image_name, width, height, 3, &float_pixel_colours[0]);
  else
  {
    std::cerr << "Error: image format " << image_ext << " not known" << std::endl;
    return false;
  }

  return true;
}

} // ray