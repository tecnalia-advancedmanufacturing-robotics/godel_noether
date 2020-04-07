#include "godel_noether/noether_path_planner.h"

#include <pluginlib/class_list_macros.h>
#include <ros/console.h>
#include <noether/surface_raster_planner_application.h>
#include <noether_conversions/noether_conversions.h>
#include <path_sequence_planner/simple_path_sequence_planner.h>
#include <eigen_conversions/eigen_msg.h>
#include <Eigen/Dense>
#include <eigen_stl_containers/eigen_stl_containers.h>
#include <tool_path_planner/utilities.h>

namespace
{

/**
 * @brief Structure to store the first and last positions of the path segments. This is all the info
 * we need to make decisions about path order as we currently don't split paths up.
 *
 * The \e a and \e b fields do not indicate any spatial relationship and are merely to uniquely
 * identify the two end points of a line segment.
 *
 * The \id field here is used to store the index of the input path that corresponds to this
 * segment. These points are sorted so this field is used to reconstruct the result at the end.
 */
struct PathEndPoints
{
  EIGEN_MAKE_ALIGNED_OPERATOR_NEW

  Eigen::Vector3d a;
  Eigen::Vector3d b;
  size_t id;
};

/**
 * @brief A structure to hold the path segments and their direction. The \e id field indicates the
 * index into the PathEndPoints sequence that corresponds to this method. The \e from_a field is used
 * to indicate whether the path should go A to B or B to A. A true value indicates A to B.
 */
struct SequencePoint
{
  size_t id;
  bool from_a;
};

/**
 * @brief From a sequence of path segments, this method extracts the end points and puts them into
 * a new reference frame. As segments are indivisible, we only need the extremes for sorting them.
 * @param segments The source of path segment data
 * @param ref_rotation A transform from the origin to a reference frame which we want all the end
 * points in. The paths in \e segments are considered to be in the origin frame.
 * @return A sequence of end points in the reference frame of \e ref_rotation
 */
std::vector<PathEndPoints> toEndPoints(const std::vector<EigenSTL::vector_Affine3d>& segments,
                                       const Eigen::Quaterniond& ref_rotation)
{
  // Ref rotation is the Target Frame w.r.t. Origin
  // The points are all w.r.t. Origin, ergo we have to pre-multiply by the inverse of ref_rotation
  // to get the new points in the Target Frame
  Eigen::Affine3d ref_inv;
  ref_inv = ref_rotation.inverse();

  std::vector<PathEndPoints> result;
  for (std::size_t i = 0; i < segments.size(); ++i)
  {
    const auto& s = segments[i];
    Eigen::Vector3d a = (ref_inv * s.front()).translation();
    Eigen::Vector3d b = (ref_inv * s.back()).translation();
    result.push_back({a, b, i});
  }
  return result;
}

/**
 * @brief Reconstructs a set of PoseArray objects using the given set of sequence points which contain
 * indices into the \e end_points array which reference the original \e in trajectory.
 * @param in The original trajectory
 * @param seqs The sequence points whose 'id' field reaches into the \e end_points vector
 * @param end_points The set of end points whose 'id' field reaches into the \e in vector
 * @return A new pose array constructed with the sequence ordering from the \e in trajectory
 */
std::vector<geometry_msgs::PoseArray> makeSequence(const std::vector<geometry_msgs::PoseArray>& in,
                                                   const std::vector<SequencePoint>& seqs,
                                                   const std::vector<PathEndPoints>& end_points)
{
  assert(in.size() == seqs.size());
  std::vector<geometry_msgs::PoseArray> rs;
  rs.reserve(in.size());

  for (const auto& seq : seqs)
  {
    rs.push_back(in[end_points[seq.id].id]); // seq.id points to end_points; end_points.id points to in
    if (!seq.from_a) // The 'in' trajectory has segments that are always A to B
    {
      std::reverse(rs.back().poses.begin(), rs.back().poses.end());
    }
  }

  return rs;
}

/**
 * @brief Computes the 'average' quaternion from an input set of them.
 * See http://stackoverflow.com/questions/12374087/average-of-multiple-quaternions
 * See http://www.acsu.buffalo.edu/~johnc/ave_quat07.pdf
 *
 * I don't have a great way of detecting the cases where the result isn't really meaningful,
 * e.g. a set of rotations spread evenly through rotational space.
 */
Eigen::Quaterniond average(const std::vector<Eigen::Quaterniond, Eigen::aligned_allocator<Eigen::Quaterniond>>& qs)
{
  Eigen::MatrixXd Q (4, qs.size());

  for (std::size_t i = 0; i < qs.size(); ++i)
  {
    Q.col(i) = qs[i].coeffs();
  }

  Eigen::MatrixXd Q_prime = Q * Q.transpose();

  Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> eigensolver(Q_prime);

  Eigen::VectorXd eigen_vals = eigensolver.eigenvalues();
  Eigen::MatrixXd eigen_vecs = eigensolver.eigenvectors();

  int max_idx = 0;
  double max_value = 0.0;
  for (int i = 0; i < eigen_vals.size(); ++i)
  {
    if (eigen_vals(i) > max_value)
    {
      max_idx = i;
      max_value = eigen_vals(i);
    }
  }

  Eigen::VectorXd coeffs = eigen_vecs.col(max_idx);
  Eigen::Quaterniond avg_quat (coeffs(3), coeffs(0), coeffs(1), coeffs(2));
  return avg_quat;
}

// Helpers to go from pose arrays to Eigen vectors of Poses
EigenSTL::vector_Affine3d toEigen(const geometry_msgs::PoseArray& p)
{
  EigenSTL::vector_Affine3d rs (p.poses.size());
  std::transform(p.poses.begin(), p.poses.end(), rs.begin(), [] (const geometry_msgs::Pose& pose)
  {
    Eigen::Affine3d e;
    tf::poseMsgToEigen(pose, e);
    return e;
  });
  return rs;
}

// Helpers to go from pose arrays to Eigen vectors of Poses
std::vector<EigenSTL::vector_Affine3d> toEigen(const std::vector<geometry_msgs::PoseArray>& ps)
{
  std::vector<EigenSTL::vector_Affine3d> rs (ps.size());
  std::transform(ps.begin(), ps.end(), rs.begin(), [] (const geometry_msgs::PoseArray& poses)
  {
    return toEigen(poses);
  });
  return rs;
}

// Gets the average quaternion rotation of a set of poses
Eigen::Quaterniond averageQuaternion(const EigenSTL::vector_Affine3d& poses)
{
  std::vector<Eigen::Quaterniond, Eigen::aligned_allocator<Eigen::Quaterniond>> qs;
  qs.reserve(poses.size());

  for (const auto& p : poses)
  {
    qs.push_back(Eigen::Quaterniond(p.rotation()));
  }

  return average(qs);
}

/**
 * @brief Returns the index of the path segment with the largest end-point displacement
 * (first.position - last.position) in \e segments
 *
 * We assume that segments is non-empty. Will return 0 in that case.
 */
std::size_t longestSegment(const std::vector<EigenSTL::vector_Affine3d>& segments)
{
  std::size_t max_index = 0;
  double max_dist = 0.0;

  for (std::size_t i = 0; i < segments.size(); ++i)
  {
    auto dist = (segments[i].front().translation() - segments[i].back().translation()).squaredNorm();
    if (dist > max_dist)
    {
      max_index = i;
      max_dist = dist;
    }
  }
  return max_index;
}

/**
 * @brief Given \e input, a set of path segments, this algorithm will produce a new set of segments
 * that is the result of re-ordering the points left to right relative to the nominal 'cut' direction.
 */
std::vector<geometry_msgs::PoseArray> sequence(const std::vector<geometry_msgs::PoseArray>& input)
{
  if (input.empty())
  {
    return {};
  }

  auto eigen_poses = toEigen(input);
  // We need to compute the 'nominal' cut direction of the surface paths
  // We do that by picking the "largest" cut first
  auto longest_segment_idx = longestSegment(eigen_poses);
  // Then we find the average rotation
  Eigen::Quaterniond avg_quaternion = averageQuaternion(eigen_poses[longest_segment_idx]);
  // And get the end points of the path segments in that rotational frame, such that paths
  // run along the X direction and are spaced out ~ in Y
  auto end_points = toEndPoints(eigen_poses, avg_quaternion);

  // Sort end points, -y to y
  std::sort(end_points.begin(), end_points.end(), [] (const PathEndPoints& lhs, const PathEndPoints& rhs)
  {
    auto lhs_value = std::min(lhs.a.y(), lhs.b.y());
    auto rhs_value = std::min(rhs.a.y(), rhs.b.y());
    return lhs_value < rhs_value;
  });

  // A helper function to get the starting point of a transition given a sequence number and
  // whether we started at A or B.
  auto current_position = [&end_points](const SequencePoint& p) {
    if (p.from_a) // If we came from A, we're now at B
      return end_points[p.id].b;
    else // if we came from B, we're not at A
      return end_points[p.id].a;
  };

  std::vector<SequencePoint> sequence;
  sequence.reserve(input.size());

  // We always start at the first end_point, position A
  sequence.push_back({0, true});

  for (std::size_t i = 1; i < end_points.size(); ++i)
  {
    // We need to determine if A or B of the next path is closer to the current position
    const Eigen::Vector3d current_pos = current_position(sequence.back());

    const auto dist_a = (end_points[i].a - current_pos).squaredNorm();
    const auto dist_b = (end_points[i].b - current_pos).squaredNorm();

    const auto from_a = dist_a < dist_b;
    sequence.push_back({i, from_a});
  }

  // Re-order the original inputs and produce a new segment.
  return makeSequence(input, sequence, end_points);
}

tool_path_planner::ProcessTool loadTool()
{
  tool_path_planner::ProcessTool tool;
  tool.pt_spacing = 0.01;
  tool.line_spacing = 0.025;
  tool.tool_offset = 0.0; // currently unused
  tool.intersecting_plane_height = 0.05; // 0.5 works best, not sure if this should be included in the tool
  //tool.nearest_neighbors = 5; // not sure if this should be a part of the tool
  tool.min_hole_size = 0.01;
  return tool;
}

std::vector<tool_path_planner::ProcessPath>
planPaths(vtkSmartPointer<vtkPolyData> mesh,
          const tool_path_planner::ProcessTool& tool)
{
  std::vector<vtkSmartPointer<vtkPolyData>> meshes;
  meshes.push_back(mesh);

  tool_path_planner::RasterToolPathPlanner planner;
  planner.setTool(tool);
  std::vector<std::vector<tool_path_planner::ProcessPath>> paths;
  planner.planPaths(meshes, paths);
  assert(paths.size() == 1);
  return paths.front();
}

} // anon namespace

void godel_noether::NoetherPathPlanner::init(pcl::PolygonMesh mesh)
{
  mesh_ = mesh;
}

bool godel_noether::NoetherPathPlanner::generatePath(const godel_msgs::PathPlanningParameters &params,
    std::vector<geometry_msgs::PoseArray> &path)
{
  ROS_INFO("Starting Noether path planning...");
  ROS_WARN("godel_noether::NoetherPathPlanner ignores the input path planning paramters");
  auto vtk_data = vtkSmartPointer<vtkPolyData>::New();
  vtk_viewer::pclEncodeMeshAndNormals(mesh_, vtk_data);
  vtk_viewer::generateNormals(vtk_data);
  ROS_INFO("generatePath: converted mesh to VTK");

  auto tool = loadTool();
  auto process_paths = planPaths(vtk_data, tool);
  ROS_INFO("generatePath: finished planning paths");

  auto paths = tool_path_planner::convertVTKtoGeometryMsgs(process_paths);
  path = sequence(paths);

  ROS_INFO("generatePath: converted to ROS messages - DONE!");

  return true;
}

PLUGINLIB_EXPORT_CLASS(godel_noether::NoetherPathPlanner, path_planning_plugins_base::PathPlanningBase)
