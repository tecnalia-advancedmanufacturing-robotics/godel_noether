#ifndef GODEL_NOETHER_PATH_PLANNER_H
#define GODEL_NOETHER_PATH_PLANNER_H

#include <path_planning_plugins_base/path_planning_base.h>
#include "godel_msgs/PathPlanningParameters.h"

namespace godel_noether
{

class NoetherPathPlanner : public path_planning_plugins_base::PathPlanningBase
{
public:
  void init(pcl::PolygonMesh mesh) override;
  bool generatePath(const godel_msgs::PathPlanningParameters &params, std::vector<geometry_msgs::PoseArray> &path) override;

private:
  pcl::PolygonMesh mesh_;
};

}

#endif // GODEL_NOETHER_PATH_PLANNER_H
