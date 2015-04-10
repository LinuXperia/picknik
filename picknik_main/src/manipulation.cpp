/*********************************************************************
 * Software License Agreement
 *
 *  Copyright (c) 2015, Dave Coleman <dave@dav.ee>
 *  All rights reserved.
 *
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 *********************************************************************/

/* Author: Dave Coleman <dave@dav.ee>
   Desc:   Manage the manipulation of MoveIt
*/

// PickNik
#include <picknik_main/manipulation.h>
#include <picknik_main/product_simulator.h>

// MoveIt
#include <moveit/ompl/model_based_planning_context.h>
#include <moveit/collision_detection/world.h>
#include <moveit/planning_pipeline/planning_pipeline.h>
#include <moveit/robot_state/conversions.h>

// OMPL
#include <ompl/tools/lightning/Lightning.h>

// C++
#include <algorithm>

// basic file operations
#include <iostream>
#include <fstream>

// Boost
#include <boost/filesystem.hpp>

namespace picknik_main
{

Manipulation::Manipulation(bool verbose, VisualsPtr visuals,
                           planning_scene_monitor::PlanningSceneMonitorPtr planning_scene_monitor,
                           ManipulationDataPtr config, GraspDatas grasp_datas,
                           RemoteControlPtr remote_control, const std::string& package_path,
                           ShelfObjectPtr shelf, bool use_experience, bool show_database)
  : nh_("~")
  , verbose_(verbose)
  , visuals_(visuals)
  , planning_scene_monitor_(planning_scene_monitor)
  , config_(config)
  , grasp_datas_(grasp_datas)
  , remote_control_(remote_control)
  , package_path_(package_path)
  , shelf_(shelf)
  , use_experience_(use_experience)
  , show_database_(show_database)
  , use_logging_(true)
{

  // Create initial robot state
  {
    planning_scene_monitor::LockedPlanningSceneRO scene(planning_scene_monitor_); // Lock planning scene
    current_state_.reset(new moveit::core::RobotState(scene->getCurrentState()));
  } // end scoped pointer of locked planning scene

  visuals_->visual_tools_->getSharedRobotState() = current_state_; // allow visual_tools to have the correct virtual joint
  robot_model_ = current_state_->getRobotModel();

  // Decide where to publish text
  status_position_ = shelf_->getBottomRight();
  bool show_text_for_video = false;
  if (show_text_for_video)
  {
    status_position_.translation().x() = 0.25;
    status_position_.translation().y() += 1.4;
    status_position_.translation().z() += shelf_->getHeight() * 0.75;
  }
  else
  {
    status_position_.translation().x() = 0.25;
    status_position_.translation().y() += shelf_->getWidth() * 0.5;
    status_position_.translation().z() += shelf_->getHeight() * 1.1;
  }
  order_position_ = status_position_;
  order_position_.translation().z() += 0.2;


  // Load logging capability
  if (use_logging_ && use_experience)
  {
    /*    if (use_thunder_ && use_experience)
          logging_file_.open("/home/dave/ompl_storage/thunder_whole_body_logging.csv", std::ios::out | std::ios::app);
          else if (use_thunder_ && !use_experience)
          logging_file_.open("/home/dave/ompl_storage/scratch_whole_body_logging.csv", std::ios::out | std::ios::app);
          else*/
    logging_file_.open("/home/dave/ompl_storage/lightning_whole_body_logging.csv", std::ios::out | std::ios::app);
  }

  // Load grasp generator
  grasp_generator_.reset( new moveit_grasps::GraspGenerator(visuals_->visual_tools_) );
  getCurrentState();
  setStateWithOpenEE(true, current_state_); // so that grasp filter is started up with EE open
  grasp_filter_.reset(new moveit_grasps::GraspFilter(current_state_, visuals_->visual_tools_) );


  // Done
  ROS_INFO_STREAM_NAMED("manipulation","Manipulation Ready.");
}

bool Manipulation::chooseGrasp(WorkOrder work_order, const robot_model::JointModelGroup* arm_jmg,
                               moveit_grasps::GraspCandidatePtr& chosen, bool verbose)
{
  BinObjectPtr& bin = work_order.bin_;
  ProductObjectPtr& product = work_order.product_;

  ROS_DEBUG_STREAM_NAMED("manipulation.superdebug","chooseGrasp()");

  Eigen::Affine3d world_to_product = product->getWorldPose(shelf_, bin);

  if (verbose)
  {
    visuals_->visual_tools_->publishAxis(world_to_product);
    visuals_->visual_tools_->publishText(world_to_product, "object_pose", rvt::BLACK, rvt::SMALL, false);
  }

  if (verbose)
  {
    std::cout << std::endl;
    std::cout << "-------------------------------------------------------" << std::endl;

    std::cout << "Before getBoundingingBoxFromMesh(): " << std::endl;
    std::cout << "  Cuboid Pose: "; printTransform(product->getCentroid());
    std::cout << "  Height: " << product->getHeight() << std::endl;
    std::cout << "  Depth: " << product->getDepth() << std::endl;
    std::cout << "  Width: " << product->getWidth() << std::endl;
  }

  // Get bounding box
  Eigen::Affine3d cuboid_pose;
  double depth, width, height;
  if (!grasp_generator_->getBoundingBoxFromMesh(product->getCollisionMesh(), cuboid_pose, depth, width, height))
  {
    ROS_ERROR_STREAM_NAMED("manipulation","Failed to get bounding box");
    return false;
  }
  product->setDepth(depth);
  product->setWidth(width);
  product->setHeight(height);

  if (verbose)
  {
    std::cout << "After getBoundingingBoxFromMesh(): " << std::endl;
    std::cout << "  Cuboid Pose: "; printTransform(product->getCentroid());
    std::cout << "  Height: " << product->getHeight() << std::endl;
    std::cout << "  Depth: " << product->getDepth() << std::endl;
    std::cout << "  Width: " << product->getWidth() << std::endl;
    std::cout << "-------------------------------------------------------" << std::endl;
  }

  // Visualize
  product->visualizeWireframe(transform(bin->getBottomRight(), shelf_->getBottomRight()));

  // Generate all possible grasps
  std::vector<moveit_msgs::Grasp> possible_grasps;

  double max_grasp_size = 0.10; // TODO: verify max object size Open Hand can grasp
  grasp_generator_->generateGrasps( world_to_product, product->getDepth(), product->getWidth(), product->getHeight(),
                                    max_grasp_size, grasp_datas_[arm_jmg], possible_grasps);

  // Convert to the correct type for filtering
  std::vector<moveit_grasps::GraspCandidatePtr> grasp_candidates;
  grasp_candidates = grasp_filter_->convertToGraspCandidatePtrs(possible_grasps,grasp_datas_[arm_jmg]);

  // Filter grasps based on IK
  bool filter_pregrasps = true;
  bool verbose_if_failed = false;
  bool grasp_verbose = false;
  if (!grasp_filter_->filterGrasps(grasp_candidates, planning_scene_monitor_, arm_jmg, filter_pregrasps, grasp_verbose,
                                   verbose_if_failed))
  {
    ROS_ERROR_STREAM_NAMED("manipulation","Unable to filter grasps");
    return false;
  }

  // Choose grasp
  if (!grasp_filter_->chooseBestGrasp(grasp_candidates, chosen))
  {
    ROS_ERROR_STREAM_NAMED("manipulation","No best grasp found");
    return false;
  }

  return true;
}

bool Manipulation::createCollisionWall()
{
  ROS_DEBUG_STREAM_NAMED("manipulation.superdebug","createCollisionWall()");

  // Clear all old collision objects
  visuals_->visual_tools_->removeAllCollisionObjects();

  // Front Wall
  shelf_->getFrontWall()->createCollisionBodies(shelf_->getBottomRight());

  // Goal bin
  shelf_->getGoalBin()->createCollisionBodies(shelf_->getBottomRight());

  // Bounding walls
  shelf_->visualizeEnvironmentObjects();

  // Output planning scene
  visuals_->visual_tools_->triggerPlanningSceneUpdate();
  ros::Duration(0.1).sleep(); // TODO remove?

  return true;
}

bool Manipulation::playbackTrajectoryFromFile(const std::string &file_name, const robot_model::JointModelGroup* arm_jmg,
                                              double velocity_scaling_factor)
{
  std::ifstream input_file;
  input_file.open (file_name.c_str());
  ROS_DEBUG_STREAM_NAMED("manipultion","Loading trajectory from file " << file_name);

  std::string line;
  current_state_ = getCurrentState();

  robot_trajectory::RobotTrajectoryPtr robot_trajectory(new robot_trajectory::RobotTrajectory(robot_model_, arm_jmg));
  double dummy_dt = 1; // temp value

  // Read each line
  while(std::getline(input_file, line))
  {
    // Convert line to a robot state
    moveit::core::RobotStatePtr new_state(new moveit::core::RobotState(*current_state_));
    moveit::core::streamToRobotState(*new_state, line, ",");
    robot_trajectory->addSuffixWayPoint(new_state, dummy_dt);
  }

  // Close file
  input_file.close();

  // Error check
  if (robot_trajectory->getWayPointCount() == 0)
  {
    ROS_ERROR_STREAM_NAMED("manipultion","No states loaded from CSV file " << file_name);
    return false;
  }

  // Interpolate between each point
  double discretization = 0.25;
  interpolate(robot_trajectory, discretization);

  // Perform iterative parabolic smoothing
  iterative_smoother_.computeTimeStamps( *robot_trajectory, velocity_scaling_factor );

  // Convert trajectory to a message
  moveit_msgs::RobotTrajectory trajectory_msg;
  robot_trajectory->getRobotTrajectoryMsg(trajectory_msg);

  std::cout << std::endl << std::endl;
  std::cout << "-------------------------------------------------------" << std::endl;
  std::cout << "MOVING ARM TO START OF TRAJECTORY" << std::endl;
  std::cout << "-------------------------------------------------------" << std::endl;

  // Plan to start state of trajectory
  bool verbose = true;
  bool execute_trajectory = true;
  ROS_INFO_STREAM_NAMED("manipulation","Moving to start state of trajectory");
  if (!move(current_state_, robot_trajectory->getFirstWayPointPtr(), arm_jmg, velocity_scaling_factor,
            verbose, execute_trajectory))
  {
    ROS_ERROR_STREAM_NAMED("manipultion","Unable to plan");
    return false;
  }

  // Visualize trajectory in Rviz display
  bool wait_for_trajetory = false;
  visuals_->visual_tools_->publishTrajectoryPath(trajectory_msg, current_state_, wait_for_trajetory);

  std::cout << std::endl;
  std::cout << "-------------------------------------------------------" << std::endl;
  std::cout << "PLAYING BACK TRAJECTORY" << std::endl;
  std::cout << "-------------------------------------------------------" << std::endl;

  // Execute
  if( !executeTrajectory(trajectory_msg) )
  {
    ROS_ERROR_STREAM_NAMED("manipulation","Failed to execute trajectory");
    return false;
  }

  return true;
}

bool Manipulation::playbackTrajectoryFromFileInteractive(const std::string &file_name, const robot_model::JointModelGroup* arm_jmg,
                                                         double velocity_scaling_factor)
{
  std::ifstream input_file;
  input_file.open (file_name.c_str());
  ROS_DEBUG_STREAM_NAMED("manipultion","Loading trajectory from file " << file_name);

  std::string line;
  current_state_ = getCurrentState();

  robot_trajectory::RobotTrajectoryPtr robot_trajectory(new robot_trajectory::RobotTrajectory(robot_model_, arm_jmg));
  double dummy_dt = 1; // temp value

  // Read each line
  while(std::getline(input_file, line))
  {
    // Convert line to a robot state
    moveit::core::RobotStatePtr new_state(new moveit::core::RobotState(*current_state_));
    moveit::core::streamToRobotState(*new_state, line, ",");
    robot_trajectory->addSuffixWayPoint(new_state, dummy_dt);
  }

  // Close file
  input_file.close();

  // Error check
  if (robot_trajectory->getWayPointCount() == 0)
  {
    ROS_ERROR_STREAM_NAMED("manipultion","No states loaded from CSV file " << file_name);
    return false;
  }

  std::cout << std::endl << std::endl;
  std::cout << "-------------------------------------------------------" << std::endl;
  std::cout << "MOVING ARM TO START OF TRAJECTORY" << std::endl;
  std::cout << "-------------------------------------------------------" << std::endl;

  // Plan to start state of trajectory
  bool verbose = true;
  bool execute_trajectory = true;
  ROS_INFO_STREAM_NAMED("manipulation","Moving to start state of trajectory");
  if (!move(current_state_, robot_trajectory->getFirstWayPointPtr(), arm_jmg, velocity_scaling_factor,
            verbose, execute_trajectory))
  {
    ROS_ERROR_STREAM_NAMED("manipultion","Unable to plan");
    return false;
  }

  // Convert trajectory to a message
  //moveit_msgs::RobotTrajectory trajectory_msg;
  //robot_trajectory->getRobotTrajectoryMsg(trajectory_msg);

  std::cout << std::endl;
  std::cout << "-------------------------------------------------------" << std::endl;
  std::cout << "PLAYING BACK TRAJECTORY" << std::endl;
  std::cout << "-------------------------------------------------------" << std::endl;

  for (std::size_t i = 0; i < robot_trajectory->getWayPointCount(); ++i)
  {
    ROS_INFO_STREAM_NAMED("manipulation","On trajectory point " << i);
    if (!executeState(robot_trajectory->getWayPointPtr(i), arm_jmg, velocity_scaling_factor))
    {
      ROS_ERROR_STREAM_NAMED("manipulation","Unable to move to next trajectory point");
      return false;
    }
  }

  return true;
}

bool Manipulation::recordTrajectoryToFile(const std::string &file_path)
{
  bool include_header = false;

  std::ofstream output_file;
  output_file.open (file_path.c_str());
  ROS_DEBUG_STREAM_NAMED("manipulation","Saving bin trajectory to file " << file_path);

  std::cout << std::endl << std::endl << std::endl;
  std::cout << "-------------------------------------------------------" << std::endl;
  std::cout << "START MOVING ARM " << std::endl;
  std::cout << "Press stop button to end recording " << std::endl;
  std::cout << "-------------------------------------------------------" << std::endl;

  std::size_t counter = 0;
  while(ros::ok() && !remote_control_->getStop())
  {
    ROS_INFO_STREAM_THROTTLE_NAMED(1, "manipulation","Recording waypoint #" << counter++ );

    moveit::core::robotStateToStream(*getCurrentState(), output_file, include_header);

    ros::Duration(0.25).sleep();
  }

  // Reset the stop button
  remote_control_->setStop(false);

  output_file.close();
  return true;
}

bool Manipulation::moveToPose(const robot_model::JointModelGroup* arm_jmg, const std::string &pose_name,
                              double velocity_scaling_factor, bool check_validity)
{
  ROS_DEBUG_STREAM_NAMED("manipulation.superdebug","moveToPose()");

  // Set new state to current state
  getCurrentState();

  // Set goal state to initial pose
  moveit::core::RobotStatePtr goal_state(new moveit::core::RobotState(*current_state_)); // Allocate robot states
  if (!goal_state->setToDefaultValues(arm_jmg, pose_name))
  {
    ROS_ERROR_STREAM_NAMED("manipulation","Failed to set pose '" << pose_name << "' for planning group '" << arm_jmg->getName() << "'");
    return false;
  }

  // Plan
  bool execute_trajectory = true;
  if (!move(current_state_, goal_state, arm_jmg, velocity_scaling_factor,
            verbose_, execute_trajectory, check_validity))
  {
    ROS_ERROR_STREAM_NAMED("manipulation","Unable to move to new position");
    return false;
  }

  return true;
}

bool Manipulation::moveEEToPose(const Eigen::Affine3d& ee_pose, double velocity_scaling_factor,
                                const robot_model::JointModelGroup* arm_jmg)
{
  ROS_DEBUG_STREAM_NAMED("manipulation.superdebug","moveEEToPose()");

  // Create start and goal
  getCurrentState();
  moveit::core::RobotStatePtr goal_state(new moveit::core::RobotState(*current_state_));

  // Setup collision checking with a locked planning scene
  {
    bool collision_checking_verbose = false;
    if (collision_checking_verbose)
      ROS_WARN_STREAM_NAMED("manipulation","moveEEToPose() has collision_checking_verbose turned on");
    boost::scoped_ptr<planning_scene_monitor::LockedPlanningSceneRO> ls;
    ls.reset(new planning_scene_monitor::LockedPlanningSceneRO(planning_scene_monitor_));
    moveit::core::GroupStateValidityCallbackFn constraint_fn
      = boost::bind(&isStateValid, static_cast<const planning_scene::PlanningSceneConstPtr&>(*ls).get(),
                    collision_checking_verbose, visuals_, _1, _2, _3);

    // Solve IK problem for arm
    std::size_t attempts = 3;
    double timeout = 0.1; // TODO
    if (!goal_state->setFromIK(arm_jmg, ee_pose, attempts, timeout, constraint_fn))
    {
      ROS_ERROR_STREAM_NAMED("manipulation","Unable to find arm solution for desired pose");
      return false;
    }
  } // end scoped pointer of locked planning scene

  ROS_INFO_STREAM_NAMED("manipulation","Found solution to pose request");

  // Debug
  //visuals_->visual_tools_->publishRobotState( goal_state, rvt::PURPLE );

  // Plan to this position
  bool verbose = true;
  bool execute_trajectory = true;
  bool check_validity = true;
  if (!move(current_state_, goal_state, arm_jmg, velocity_scaling_factor, verbose, execute_trajectory, check_validity))
  {
    ROS_ERROR_STREAM_NAMED("manipulation","Failed to move EE to desired pose");
    return false;
  }

  ROS_INFO_STREAM_NAMED("manipulation","Moved EE to desired pose successfullly");

  return true;
}

bool Manipulation::move(const moveit::core::RobotStatePtr& start, const moveit::core::RobotStatePtr& goal,
                        const robot_model::JointModelGroup* arm_jmg, double velocity_scaling_factor,
                        bool verbose, bool execute_trajectory, bool check_validity)
{
  ROS_INFO_STREAM_NAMED("manipulation.move","Planning to new pose with velocity scale " << velocity_scaling_factor);

  // Check validity of start and goal
  if (check_validity && !checkCollisionAndBounds(start, goal))
  {
    ROS_ERROR_STREAM_NAMED("manipulation","Potential issue with start and goal state, but perhaps this should not fail in the future");
    return false;
  }
  else if (!check_validity)
    ROS_WARN_STREAM_NAMED("manipulation","Start/goal state collision checking for move() was disabled");

  // Visualize start and goal
  if (verbose)
  {
    visuals_->start_state_->publishRobotState(start, rvt::GREEN);
    visuals_->goal_state_->publishRobotState(goal, rvt::ORANGE);
  }

  // Check if already in new position
  if (statesEqual(*start, *goal, arm_jmg))
  {
    ROS_INFO_STREAM_NAMED("manipulation","Not planning motion because current state and goal state are close enough.");
    return true;
  }

  // Create motion planning request
  planning_interface::MotionPlanRequest req;
  planning_interface::MotionPlanResponse res;

  moveit::core::robotStateToRobotStateMsg(*start, req.start_state);

  // Create Goal constraint
  double tolerance_pose = 0.0001;
  moveit_msgs::Constraints goal_constraint = kinematic_constraints::constructGoalConstraints(*goal, arm_jmg,
                                                                                             tolerance_pose, tolerance_pose);
  req.goal_constraints.push_back(goal_constraint);

  // Other settings e.g. OMPL
  req.planner_id = "RRTConnectkConfigDefault";
  //req.planner_id = "RRTstarkConfigDefault";
  req.group_name = arm_jmg->getName();
  if (use_experience_)
    req.num_planning_attempts = 1; // this must be one else it threads and doesn't use lightning/thunder correctly
  else
    req.num_planning_attempts = 3; // this is also the number of threads to use
  req.allowed_planning_time = 30; // seconds
  req.use_experience = use_experience_;
  req.experience_method = "lightning";
  req.max_velocity_scaling_factor = velocity_scaling_factor;

  // Parameters for the workspace that the planner should work inside relative to center of robot
  double workspace_size = 1;
  req.workspace_parameters.header.frame_id = robot_model_->getModelFrame();
  req.workspace_parameters.min_corner.x = start->getVariablePosition("virtual_joint/trans_x") - workspace_size;
  req.workspace_parameters.min_corner.y = start->getVariablePosition("virtual_joint/trans_y") - workspace_size;
  req.workspace_parameters.min_corner.z = 0; //floor start->getVariablePosition("virtual_joint/trans_z") - workspace_size;
  req.workspace_parameters.max_corner.x = start->getVariablePosition("virtual_joint/trans_x") + workspace_size;
  req.workspace_parameters.max_corner.y = start->getVariablePosition("virtual_joint/trans_y") + workspace_size;
  req.workspace_parameters.max_corner.z = start->getVariablePosition("virtual_joint/trans_z") + workspace_size;

  //visuals_->visual_tools_->publishWorkspaceParameters(req.workspace_parameters);

  // Call pipeline
  std::vector<std::size_t> dummy;
  planning_interface::PlanningContextPtr planning_context_handle;

  // SOLVE
  loadPlanningPipeline(); // always call before using generatePlan()
  planning_scene::PlanningScenePtr cloned_scene;
  {
    planning_scene_monitor::LockedPlanningSceneRO scene(planning_scene_monitor_); // Lock planning scene
    cloned_scene = planning_scene::PlanningScene::clone(scene);
  } // end scoped pointer of locked planning scene
  planning_pipeline_->generatePlan(cloned_scene, req, res, dummy, planning_context_handle);

  // Get the trajectory
  moveit_msgs::MotionPlanResponse response;
  response.trajectory = moveit_msgs::RobotTrajectory();
  res.getMessage(response);

  // Check that the planning was successful
  bool error = (res.error_code_.val != res.error_code_.SUCCESS);
  if (error)
  {
    std::string result = getActionResultString(res.error_code_, response.trajectory.joint_trajectory.points.empty());
    ROS_ERROR_STREAM_NAMED("manipulation","Planning failed:: " << result);
  }

  // Visualize trajectory in Rviz display
  bool wait_for_trajetory = false;
  visuals_->visual_tools_->publishTrajectoryPath(response.trajectory, current_state_, wait_for_trajetory);

  // Focus on execution (unless we are in debug mode)
  if (!error)
    visuals_->visual_tools_->hideRobot();

  // Do not execute a bad trajectory
  if (error)
    return false;

  // Hack: do not allow a two point trajectory to be executed because there is no velcity?
  if (response.trajectory.joint_trajectory.points.size() < 3)
  {
    ROS_ERROR_STREAM_NAMED("manipulation","Trajectory only has " << response.trajectory.joint_trajectory.points.size() << " points");

    if (response.trajectory.joint_trajectory.points.size() == 2)
    {
      // Remove previous parameterization
      for (std::size_t i = 0; i < response.trajectory.joint_trajectory.points.size(); ++i)
      {
        response.trajectory.joint_trajectory.points[i].velocities.clear();
        response.trajectory.joint_trajectory.points[i].accelerations.clear();
      }

      // Add more waypoints
      robot_trajectory::RobotTrajectoryPtr robot_trajectory(new robot_trajectory::RobotTrajectory(robot_model_, arm_jmg));
      robot_trajectory->setRobotTrajectoryMsg(*current_state_, response.trajectory);

      // Interpolate
      double discretization = 0.25;
      interpolate(robot_trajectory, discretization);

      // Convert trajectory back to a message
      robot_trajectory->getRobotTrajectoryMsg(response.trajectory);

      std::cout << "BEFORE PARAM: \n" << response.trajectory << std::endl;

      // Perform iterative parabolic smoothing
      iterative_smoother_.computeTimeStamps( *robot_trajectory, config_->main_velocity_scaling_factor_ );

      // Convert trajectory back to a message
      robot_trajectory->getRobotTrajectoryMsg(response.trajectory);
    }
  }

  // Execute trajectory
  if (execute_trajectory)
  {
    if( !executeTrajectory(response.trajectory) )
    {
      ROS_ERROR_STREAM_NAMED("manipulation","Failed to execute trajectory");
      return false;
    }
  }
  else
  {
    ROS_WARN_STREAM_NAMED("manipulation","Execute trajectory currently disabled by user");
  }

  // Save Experience Database
  if (use_experience_)
  {
    moveit_ompl::ModelBasedPlanningContextPtr mbpc
      = boost::dynamic_pointer_cast<moveit_ompl::ModelBasedPlanningContext>(planning_context_handle);
    ompl::tools::ExperienceSetupPtr experience_setup
      = boost::dynamic_pointer_cast<ompl::tools::ExperienceSetup>(mbpc->getOMPLSimpleSetup());


    // Display logs
    experience_setup->printLogs();

    // Logging
    if (use_logging_)
    {
      experience_setup->saveDataLog(logging_file_);
      logging_file_.flush();
    }

    // Save database
    ROS_INFO_STREAM_NAMED("manipulation","Saving experience db...");
    experience_setup->saveIfChanged();

    // Show the database
    if (show_database_)
    {
      ROS_ERROR_STREAM_NAMED("manipulation","Showing database...");
      displayLightningPlans(experience_setup, arm_jmg);
    }
  }

  if (error)
  {
    return false;
  }
  return true;
}

bool Manipulation::interpolate(robot_trajectory::RobotTrajectoryPtr robot_trajectory, const double& discretization)
{
  double dummy_dt = 1; // dummy value until parameterization

  robot_trajectory::RobotTrajectoryPtr new_robot_trajectory(new robot_trajectory::RobotTrajectory(robot_model_,
                                                                                                  robot_trajectory->getGroup()));

  std::size_t original_num_waypoints = robot_trajectory->getWayPointCount();

  // For each set of points (A,B) in the original trajectory
  for (std::size_t i = 0; i < robot_trajectory->getWayPointCount() - 1; ++i)
  {
    // Add point A to final trajectory
    new_robot_trajectory->addSuffixWayPoint(robot_trajectory->getWayPoint(i), dummy_dt);

    for (double t = discretization; t < 1; t += discretization)
    {
      // Create new state
      moveit::core::RobotStatePtr interpolated_state(new moveit::core::RobotState(robot_trajectory->getFirstWayPoint()));
      // Fill in new values
      robot_trajectory->getWayPoint(i).interpolate(robot_trajectory->getWayPoint(i+1), t, *interpolated_state);
      // Add to trajectory
      new_robot_trajectory->addSuffixWayPoint(interpolated_state, dummy_dt);
      //std::cout << "inserting " << t << " at " << new_robot_trajectory->getWayPointCount() << std::endl;
    }
  }

  // Add final waypoint
  new_robot_trajectory->addSuffixWayPoint(robot_trajectory->getLastWayPoint(), dummy_dt);

  std::size_t modified_num_waypoints = new_robot_trajectory->getWayPointCount();
  ROS_INFO_STREAM_NAMED("manipulation","Interpolated trajectory from " << original_num_waypoints 
                        << " to " << modified_num_waypoints);

  // Copy back to original datastructure
  *robot_trajectory = *new_robot_trajectory;

  return true;
}

std::string Manipulation::getActionResultString(const moveit_msgs::MoveItErrorCodes &error_code, bool planned_trajectory_empty)
{
  if (error_code.val == moveit_msgs::MoveItErrorCodes::SUCCESS)
  {
    if (planned_trajectory_empty)
      return "Requested path and goal constraints are already met.";
    else
    {
      return "Solution was found and executed.";
    }
  }
  else
    if (error_code.val == moveit_msgs::MoveItErrorCodes::INVALID_GROUP_NAME)
      return "Must specify group in motion plan request";
    else
      if (error_code.val == moveit_msgs::MoveItErrorCodes::PLANNING_FAILED || error_code.val == moveit_msgs::MoveItErrorCodes::INVALID_MOTION_PLAN)
      {
        if (planned_trajectory_empty)
          return "No motion plan found. No execution attempted.";
        else
          return "Motion plan was found but it seems to be invalid (possibly due to postprocessing). Not executing.";
      }
      else
        if (error_code.val == moveit_msgs::MoveItErrorCodes::UNABLE_TO_AQUIRE_SENSOR_DATA)
          return "Motion plan was found but it seems to be too costly and looking around did not help.";
        else
          if (error_code.val == moveit_msgs::MoveItErrorCodes::MOTION_PLAN_INVALIDATED_BY_ENVIRONMENT_CHANGE)
            return "Solution found but the environment changed during execution and the path was aborted";
          else
            if (error_code.val == moveit_msgs::MoveItErrorCodes::CONTROL_FAILED)
              return "Solution found but controller failed during execution";
            else
              if (error_code.val == moveit_msgs::MoveItErrorCodes::TIMED_OUT)
                return "Timeout reached";
              else
                if (error_code.val == moveit_msgs::MoveItErrorCodes::PREEMPTED)
                  return "Preempted";
                else
                  if (error_code.val == moveit_msgs::MoveItErrorCodes::INVALID_GOAL_CONSTRAINTS)
                    return "Invalid goal constraints";
                  else
                    if (error_code.val == moveit_msgs::MoveItErrorCodes::INVALID_OBJECT_NAME)
                      return "Invalid object name";
                    else
                      if (error_code.val == moveit_msgs::MoveItErrorCodes::FAILURE)
                        return "Catastrophic failure";
  return "Unknown event";
}

bool Manipulation::executeState(const moveit::core::RobotStatePtr goal_state, const moveit::core::JointModelGroup *jmg,
                                double velocity_scaling_factor)
{
  ROS_DEBUG_STREAM_NAMED("manipulation.superdebug","executeState()");

  // Get the start state
  getCurrentState();

  // Visualize start/goal
  visuals_->start_state_->publishRobotState(current_state_, rvt::GREEN);
  visuals_->goal_state_->publishRobotState(goal_state, rvt::ORANGE);

  // Create trajectory
  std::vector<moveit::core::RobotStatePtr> robot_state_trajectory;
  robot_state_trajectory.push_back(current_state_);

  // Create an interpolated trajectory between states
  double resolution = 0.1;
  for (double t = 0; t < 1; t += resolution)
  {
    moveit::core::RobotStatePtr interpolated_state = moveit::core::RobotStatePtr(new moveit::core::RobotState(*current_state_));
    current_state_->interpolate(*goal_state, t, *interpolated_state);
    robot_state_trajectory.push_back(interpolated_state);
  }
  // Add goal state
  robot_state_trajectory.push_back(goal_state);

  // Get trajectory message
  moveit_msgs::RobotTrajectory trajectory_msg;
  if (!convertRobotStatesToTrajectory(robot_state_trajectory, trajectory_msg, jmg, velocity_scaling_factor))
  {
    ROS_ERROR_STREAM_NAMED("manipulation","Failed to convert to parameterized trajectory");
    return false;
  }

  // Visualize trajectory in Rviz display
  bool wait_for_trajetory = false;
  visuals_->visual_tools_->publishTrajectoryPath(trajectory_msg, current_state_, wait_for_trajetory);

  // Execute
  if( !executeTrajectory(trajectory_msg) )
  {
    ROS_ERROR_STREAM_NAMED("manipulation","Failed to execute trajectory");
    return false;
  }
  return true;
}

bool Manipulation::generateApproachPath(moveit_grasps::GraspCandidatePtr chosen,
                                        moveit_msgs::RobotTrajectory &approach_trajectory_msg,
                                        const moveit::core::RobotStatePtr pre_grasp_state,
                                        const moveit::core::RobotStatePtr the_grasp_state,
                                        bool verbose)
{
  ROS_DEBUG_STREAM_NAMED("manipulation.superdebug","generateApproachPath()");

  ROS_DEBUG_STREAM_NAMED("manipulation.generate_approach_path","finger_to_palm_depth: " << chosen->grasp_data_->finger_to_palm_depth_);
  ROS_DEBUG_STREAM_NAMED("manipulation.generate_approach_path","approach_distance_desired: " << config_->approach_distance_desired_);
  double desired_distance = chosen->grasp_data_->finger_to_palm_depth_ + config_->approach_distance_desired_;

  Eigen::Vector3d direction = grasp_generator_->getPreGraspDirection(chosen->grasp_, chosen->grasp_data_->parent_link_->getName());
  //            x, y, z
  //direction << -1, 0, 0.5; // backwards towards robot body
  //std::cout << "DIRECTION: " << direction << std::endl;
  bool reverse_path = true;

  double path_length;
  bool ignore_collision = false;
  std::vector<moveit::core::RobotStatePtr> robot_state_trajectory;
  getCurrentState();
  if (!computeStraightLinePath( direction, desired_distance, robot_state_trajectory, the_grasp_state, chosen->grasp_data_->arm_jmg_,
                                reverse_path, path_length, ignore_collision))
  {
    ROS_ERROR_STREAM_NAMED("manipulation","Error occured while computing straight line path");
    return false;
  }

  // Get approach trajectory message
  if (!convertRobotStatesToTrajectory(robot_state_trajectory, approach_trajectory_msg, chosen->grasp_data_->arm_jmg_,
                                      config_->approach_velocity_scaling_factor_))
  {
    ROS_ERROR_STREAM_NAMED("manipulation","Failed to convert to parameterized trajectory");
    return false;
  }

  // Visualize trajectory in Rviz display
  bool wait_for_trajetory = false;
  visuals_->visual_tools_->publishTrajectoryPath(approach_trajectory_msg, current_state_, wait_for_trajetory);

  // Set the pregrasp to be the first state in the trajectory. Copy value, not pointer
  *pre_grasp_state = *first_state_in_trajectory_;

  visuals_->visual_tools_->publishRobotState(pre_grasp_state, rvt::PURPLE);

  return true;
}

bool Manipulation::executeVerticlePath(const moveit::core::JointModelGroup *arm_jmg, const double &desired_lift_distance, bool up,
                                       bool ignore_collision)
{
  ROS_DEBUG_STREAM_NAMED("manipulation.superdebug","executeVerticlePath()");

  Eigen::Vector3d approach_direction;
  approach_direction << 0, 0, (up ? 1 : -1); // 1 is up, -1 is down
  bool reverse_path = false;

  if (!executeCartesianPath(arm_jmg, approach_direction, desired_lift_distance, config_->lift_velocity_scaling_factor_, reverse_path,
                            ignore_collision))
  {
    ROS_ERROR_STREAM_NAMED("manipulation","Failed to execute horizontal path");
    return false;
  }
  return true;
}

bool Manipulation::executeHorizontalPath(const moveit::core::JointModelGroup *arm_jmg, const double &desired_lift_distance, bool left,
                                         bool ignore_collision)
{
  ROS_DEBUG_STREAM_NAMED("manipulation.superdebug","executeHorizontalPath()");

  Eigen::Vector3d approach_direction;
  approach_direction << 0, (left ? 1 : -1), 0; // 1 is left, -1 is right
  bool reverse_path = false;

  if (!executeCartesianPath(arm_jmg, approach_direction, desired_lift_distance, config_->lift_velocity_scaling_factor_, reverse_path,
                            ignore_collision))
  {
    ROS_ERROR_STREAM_NAMED("manipulation","Failed to execute horizontal path");
    return false;
  }
  return true;
}

bool Manipulation::executeRetreatPath(const moveit::core::JointModelGroup *arm_jmg, double desired_approach_distance, bool retreat,
                                      bool ignore_collision)
{
  ROS_DEBUG_STREAM_NAMED("manipulation.superdebug","executeRetreatPath()");

  // Compute straight line in reverse from grasp
  Eigen::Vector3d approach_direction;
  approach_direction << (retreat ? -1 : 1), 0, 0; // backwards towards robot body
  bool reverse_path = false;

  if (!executeCartesianPath(arm_jmg, approach_direction, desired_approach_distance, config_->retreat_velocity_scaling_factor_,
                            reverse_path, ignore_collision))
  {
    ROS_ERROR_STREAM_NAMED("manipulation","Failed to execute retreat path");
    return false;
  }
  return true;
}

bool Manipulation::executeCartesianPath(const moveit::core::JointModelGroup *arm_jmg, const Eigen::Vector3d& direction,
                                        double desired_distance, double velocity_scaling_factor,
                                        bool reverse_path, bool ignore_collision)
{
  ROS_DEBUG_STREAM_NAMED("manipulation.superdebug","executeCartesianPath()");
  getCurrentState();

  double path_length;
  std::vector<moveit::core::RobotStatePtr> robot_state_trajectory;
  if (!computeStraightLinePath( direction, desired_distance, robot_state_trajectory, current_state_, arm_jmg, reverse_path,
                                path_length, ignore_collision))

  {
    ROS_ERROR_STREAM_NAMED("manipulation","Error occured while computing straight line path");
    return false;
  }

  // Get approach trajectory message
  moveit_msgs::RobotTrajectory cartesian_trajectory_msg;
  if (!convertRobotStatesToTrajectory(robot_state_trajectory, cartesian_trajectory_msg, arm_jmg, velocity_scaling_factor))
  {
    ROS_ERROR_STREAM_NAMED("manipulation","Failed to convert to parameterized trajectory");
    return false;
  }

  // Visualize trajectory in Rviz display
  bool wait_for_trajetory = false;
  visuals_->visual_tools_->publishTrajectoryPath(cartesian_trajectory_msg, current_state_, wait_for_trajetory);

  // Execute
  if( !executeTrajectory(cartesian_trajectory_msg, ignore_collision) )
  {
    ROS_ERROR_STREAM_NAMED("manipulation","Failed to execute trajectory");
    return false;
  }

  return true;
}

bool Manipulation::computeStraightLinePath( Eigen::Vector3d approach_direction,
                                            double desired_approach_distance,
                                            std::vector<moveit::core::RobotStatePtr>& robot_state_trajectory,
                                            moveit::core::RobotStatePtr robot_state,
                                            const moveit::core::JointModelGroup *arm_jmg,
                                            bool reverse_trajectory,
                                            double& path_length,
                                            bool ignore_collision)
{
  ROS_DEBUG_STREAM_NAMED("manipulation.superdebug","computeStraightLinePath()");

  if (ignore_collision)
    ROS_INFO_STREAM_NAMED("manipulation","computeStraightLinePath() is ignoring collisions");

  // End effector parent link (arm tip for ik solving)
  const moveit::core::LinkModel *ik_tip_link_model = grasp_datas_[arm_jmg]->parent_link_;

  // ---------------------------------------------------------------------------------------------
  // Show desired trajectory in BLACK
  Eigen::Affine3d tip_pose_start = robot_state->getGlobalLinkTransform(ik_tip_link_model);

  // Debug
  if (false)
  {
    std::cout << "Tip Pose Start \n" << tip_pose_start.translation().x() << "\t"
              << tip_pose_start.translation().y()
              << "\t" << tip_pose_start.translation().z() << std::endl;
  }

  // Visualize start and goal state
  if (verbose_)
  {
    visuals_->visual_tools_->publishSphere(tip_pose_start, rvt::RED, rvt::LARGE);

    Eigen::Affine3d tip_pose_end; // = tip_pose_start;
    //tip_pose_end.translation().x() -= desired_approach_distance;

    straightProjectPose( tip_pose_start, tip_pose_end, approach_direction, desired_approach_distance);

    visuals_->visual_tools_->publishLine(tip_pose_start, tip_pose_end, rvt::BLACK, rvt::REGULAR);

    // Show start and goal states of cartesian path
    if (reverse_trajectory)
    {
      // The passed in robot state is the goal
      visuals_->start_state_->hideRobot();
      visuals_->goal_state_->publishRobotState(robot_state, rvt::ORANGE);
    }
    else
    {
      // The passed in robot state is the start (retreat)
      visuals_->goal_state_->hideRobot();
      visuals_->start_state_->publishRobotState(robot_state, rvt::GREEN);
    }
  }

  // ---------------------------------------------------------------------------------------------
  // Settings for computeCartesianPath

  // Resolution of trajectory
  double max_step = 0.01; // 0.01 // The maximum distance in Cartesian space between consecutive points on the resulting path

  // Error check
  if (desired_approach_distance < max_step)
  {
    ROS_ERROR_STREAM_NAMED("manipulation","Not enough: desired_approach_distance (" << desired_approach_distance << ")  < max_step (" << max_step << ")");
    return false;
  }

  // Jump threshold for preventing consequtive joint values from 'jumping' by a large amount in joint space
  double jump_threshold = 0.0; // disabled

  bool collision_checking_verbose = false;

  // Check for kinematic solver
  if( !arm_jmg->canSetStateFromIK( ik_tip_link_model->getName() ) )
    ROS_ERROR_STREAM_NAMED("manipulation","No IK Solver loaded - make sure moveit_config/kinamatics.yaml is loaded in this namespace");

  {
    // Collision check
    boost::scoped_ptr<planning_scene_monitor::LockedPlanningSceneRO> ls;
    ls.reset(new planning_scene_monitor::LockedPlanningSceneRO(planning_scene_monitor_));
    moveit::core::GroupStateValidityCallbackFn constraint_fn
      = boost::bind(&isStateValid, static_cast<const planning_scene::PlanningSceneConstPtr&>(*ls).get(),
                    collision_checking_verbose, visuals_, _1, _2, _3);

    // -----------------------------------------------------------------------------------------------
    // Compute Cartesian Path
    path_length = robot_state->computeCartesianPath(arm_jmg,
                                                    robot_state_trajectory,
                                                    ik_tip_link_model,
                                                    approach_direction,
                                                    true,           // direction is in global reference frame
                                                    desired_approach_distance,
                                                    max_step,
                                                    jump_threshold,
                                                    ignore_collision ? NULL : constraint_fn // collision check
                                                    );

    ROS_DEBUG_STREAM_NAMED("manipulation","Cartesian resulting distance: " << path_length << " desired: " << desired_approach_distance
                           << " number of states in trajectory: " << robot_state_trajectory.size());

    if( path_length == 0 )
    {
      ROS_ERROR_STREAM_NAMED("manipulation","Failed to computer cartesian path: distance is 0. Displaying collision debug information:");

      // Recreate collision checker callback
      collision_checking_verbose = true;
      constraint_fn = boost::bind(&isStateValid, static_cast<const planning_scene::PlanningSceneConstPtr&>(*ls).get(),
                                  collision_checking_verbose, visuals_, _1, _2, _3);

      // Re-compute Cartesian Path
      path_length = robot_state->computeCartesianPath(arm_jmg,
                                                      robot_state_trajectory,
                                                      ik_tip_link_model,
                                                      approach_direction,
                                                      true,           // direction is in global reference frame
                                                      desired_approach_distance,
                                                      max_step,
                                                      jump_threshold,
                                                      constraint_fn // collision check
                                                      );
      return false;
    }
    else if ( path_length < desired_approach_distance * 0.5 )
    {
      ROS_WARN_STREAM_NAMED("manipulation","Resuling cartesian path distance is less than half the desired distance");
    }
  } // end scoped pointer of locked planning scene

  // Reverse the trajectory if neeeded
  if (reverse_trajectory)
  {
    std::reverse(robot_state_trajectory.begin(), robot_state_trajectory.end());

    // Also, save the first state so that generateApproachPath() can use it
    first_state_in_trajectory_ = robot_state_trajectory.front();
  }

  // Debug
  if (verbose_)
  {
    // Super debug
    if (false)
    {
      std::cout << "Tip Pose Result: \n";
      for (std::size_t i = 0; i < robot_state_trajectory.size(); ++i)
      {
        const Eigen::Affine3d tip_pose_start = robot_state_trajectory[i]->getGlobalLinkTransform(ik_tip_link_model);
        std::cout << tip_pose_start.translation().x() << "\t" << tip_pose_start.translation().y() <<
          "\t" << tip_pose_start.translation().z() << std::endl;
      }
    }

    // Show actual trajectory in GREEN
    ROS_INFO_STREAM_NAMED("manipulation","Displaying cartesian trajectory in green");
    const Eigen::Affine3d& tip_pose_end =
      robot_state_trajectory.back()->getGlobalLinkTransform(ik_tip_link_model);
    visuals_->visual_tools_->publishLine(tip_pose_start, tip_pose_end, rvt::LIME_GREEN, rvt::LARGE);
    visuals_->visual_tools_->publishSphere(tip_pose_end, rvt::ORANGE, rvt::LARGE);

    // Visualize end effector position of cartesian path
    ROS_INFO_STREAM_NAMED("manipulation","Visualize end effector position of cartesian path");
    visuals_->visual_tools_->publishTrajectoryPoints(robot_state_trajectory, ik_tip_link_model);


    // Show start and goal states of cartesian path
    if (reverse_trajectory)
    {
      // The passed in robot state is the goal
      visuals_->start_state_->publishRobotState(robot_state_trajectory.front(), rvt::GREEN);
    }
    else
    {
      // The passed in robot state is the start (retreat)
      visuals_->goal_state_->publishRobotState(robot_state_trajectory.back(), rvt::ORANGE);
    }
  }


  return true;
}

const robot_model::JointModelGroup* Manipulation::chooseArm(const Eigen::Affine3d& ee_pose)
{
  // Single Arm
  if (!config_->dual_arm_)
  {
    return config_->right_arm_; // right is always the default arm for single arm robots
  }
  // Dual Arm
  else if (ee_pose.translation().y() < 0)
  {
    ROS_DEBUG_STREAM_NAMED("manipulation","Using right arm for task");
    return config_->right_arm_;
  }
  else
  {
    ROS_DEBUG_STREAM_NAMED("manipulation","Using left arm for task");
    return config_->left_arm_;
  }
}

bool Manipulation::perturbCamera(BinObjectPtr bin)
{
  // Note: assumes arm is already pointing at centroid of desired bin
  ROS_INFO_STREAM_NAMED("manipulation","Perturbing camera for perception");

  // Choose which arm to utilize for task
  Eigen::Affine3d ee_pose = transform(bin->getCentroid(), shelf_->getBottomRight()); // convert to world coordinates
  const robot_model::JointModelGroup* arm_jmg = chooseArm(ee_pose);

  //Move camera left
  std::cout << std::endl;
  std::cout << "-------------------------------------------------------" << std::endl;
  ROS_INFO_STREAM_NAMED("manipulation","Moving camera left distance " << config_->camera_left_distance_);
  bool left = true;
  if (!executeHorizontalPath(arm_jmg, config_->camera_left_distance_, left))
  {
    ROS_ERROR_STREAM_NAMED("manipulation","Unable to move left");
  }

  // Move camera right
  std::cout << std::endl;
  std::cout << "-------------------------------------------------------" << std::endl;
  ROS_INFO_STREAM_NAMED("manipulation","Moving camera right distance " << config_->camera_left_distance_ * 2.0);
  left = false;
  if (!executeHorizontalPath(arm_jmg, config_->camera_left_distance_ * 2.0, left))
  {
    ROS_ERROR_STREAM_NAMED("manipulation","Unable to move right");
  }

  // Move back to center
  std::cout << std::endl;
  std::cout << "-------------------------------------------------------" << std::endl;
  if (!moveCameraToBin(bin))
  {
    ROS_ERROR_STREAM_NAMED("manipulation","Unable to move camera to bin " << bin->getName());
    return false;
  }

  // Move camera up
  std::cout << std::endl;
  std::cout << "-------------------------------------------------------" << std::endl;
  ROS_INFO_STREAM_NAMED("manipulation","Lifting camera distance " << config_->camera_lift_distance_);
  if (!executeVerticlePath(arm_jmg, config_->camera_lift_distance_))
  {
    ROS_ERROR_STREAM_NAMED("manipulation","Unable to move up");
  }

  // Move camera down
  std::cout << std::endl;
  std::cout << "-------------------------------------------------------" << std::endl;
  ROS_INFO_STREAM_NAMED("manipulation","Lowering camera distance " << config_->camera_lift_distance_ * 2.0);
  bool up = false;
  if (!executeVerticlePath(arm_jmg, config_->camera_lift_distance_ * 2.0, up))
  {
    ROS_ERROR_STREAM_NAMED("manipulation","Unable to move down");
  }

  return true;
}

bool Manipulation::moveCameraToBin(BinObjectPtr bin)
{
  // Create pose to find IK solver
  Eigen::Affine3d ee_pose = transform(bin->getCentroid(), shelf_->getBottomRight()); // convert to world coordinates

  // Move centroid backwards
  ee_pose.translation().x() += config_->camera_x_translation_from_bin_;
  ee_pose.translation().y() += config_->camera_y_translation_from_bin_;
  ee_pose.translation().z() += config_->camera_z_translation_from_bin_;

  // Convert pose that has x arrow pointing to object, to pose that has z arrow pointing towards object and x out in the grasp dir
  ee_pose = ee_pose * Eigen::AngleAxisd(M_PI/2.0, Eigen::Vector3d::UnitY());
  ee_pose = ee_pose * Eigen::AngleAxisd(M_PI, Eigen::Vector3d::UnitZ());

  // Debug
  visuals_->visual_tools_->publishAxis(ee_pose);
  visuals_->visual_tools_->publishText(ee_pose, "ee_pose", rvt::BLACK, rvt::SMALL, false);

  // Choose which arm to utilize for task
  const robot_model::JointModelGroup* arm_jmg = chooseArm(ee_pose);

  // Translate to custom end effector geometry
  ee_pose = ee_pose * grasp_datas_[arm_jmg]->grasp_pose_to_eef_pose_;

  // Customize the direction it is pointing
  // Roll Angle
  ee_pose = ee_pose * Eigen::AngleAxisd(config_->camera_z_rotation_from_standard_grasp_, Eigen::Vector3d::UnitZ());
  // Pitch Angle
  ee_pose = ee_pose * Eigen::AngleAxisd(config_->camera_x_rotation_from_standard_grasp_, Eigen::Vector3d::UnitX());
  // Yaw Angle
  ee_pose = ee_pose * Eigen::AngleAxisd(config_->camera_y_rotation_from_standard_grasp_, Eigen::Vector3d::UnitY());

  return moveEEToPose(ee_pose, config_->main_velocity_scaling_factor_, arm_jmg);
}

bool Manipulation::straightProjectPose( const Eigen::Affine3d& original_pose, Eigen::Affine3d& new_pose,
                                        const Eigen::Vector3d direction, double distance)
{
  ROS_DEBUG_STREAM_NAMED("manipulation.superdebug","straightProjectPose()");

  // Assume everything is in world coordinates

  new_pose = original_pose;

  Eigen::Vector3d longer_direction = direction * distance;

  new_pose.translation() += longer_direction;

  return true;
}

bool Manipulation::convertRobotStatesToTrajectory(const std::vector<moveit::core::RobotStatePtr>& robot_state_trajectory,
                                                  moveit_msgs::RobotTrajectory& trajectory_msg,
                                                  const robot_model::JointModelGroup* jmg,
                                                  const double &velocity_scaling_factor)
{
  ROS_DEBUG_STREAM_NAMED("manipulation.superdebug","convertRobotStatesToTrajectory()");

  // Copy the vector of RobotStates to a RobotTrajectory
  robot_trajectory::RobotTrajectoryPtr robot_trajectory(new robot_trajectory::RobotTrajectory(robot_model_, jmg));

  // -----------------------------------------------------------------------------------------------
  // Convert o RobotTrajectory datatype
  for (std::size_t k = 0 ; k < robot_state_trajectory.size() ; ++k)
  {
    double duration_from_previous = 1; // this is overwritten and unimportant
    robot_trajectory->addSuffixWayPoint(robot_state_trajectory[k], duration_from_previous);
  }

  // Interpolate any path with two few points
  static const std::size_t MIN_TRAJECTORY_POINTS = 6;
  if (robot_trajectory->getWayPointCount() < MIN_TRAJECTORY_POINTS)
  {
    ROS_WARN_STREAM_NAMED("manipulation","Interpolating trajectory because two few points (" << robot_trajectory->getWayPointCount() << ")");

    // Interpolate between each point
    double discretization = 0.25;
    interpolate(robot_trajectory, discretization);    
  }
  
  // Perform iterative parabolic smoothing
  iterative_smoother_.computeTimeStamps( *robot_trajectory, velocity_scaling_factor );

  // Convert trajectory to a message
  robot_trajectory->getRobotTrajectoryMsg(trajectory_msg);

  return true;
}

bool Manipulation::openEndEffectors(bool open)
{
  ROS_DEBUG_STREAM_NAMED("manipulation.superdebug","openEndEffectors()");

  openEndEffectorWithVelocity(true, config_->right_arm_);
  if (config_->dual_arm_)
    openEndEffectorWithVelocity(true, config_->left_arm_);
  return true;
}

bool Manipulation::openEndEffector(bool open, const robot_model::JointModelGroup* arm_jmg)
{
  ROS_DEBUG_STREAM_NAMED("manipulation.superdebug","openEndEffector()");
  ROS_ERROR_STREAM_NAMED("temp","THIS FUNCTION IS DPERECATED");

  getCurrentState();
  const robot_model::JointModelGroup* ee_jmg = grasp_datas_[arm_jmg]->ee_jmg_;

  robot_trajectory::RobotTrajectoryPtr ee_traj(new robot_trajectory::RobotTrajectory(robot_model_, ee_jmg));

  if (open)
  {
    ROS_INFO_STREAM_NAMED("manipulation","Opening end effector for " << grasp_datas_[arm_jmg]->ee_jmg_->getName());
    ee_traj->setRobotTrajectoryMsg(*current_state_, grasp_datas_[arm_jmg]->pre_grasp_posture_); // open
  }
  else
  {
    ROS_INFO_STREAM_NAMED("manipulation","Closing end effector for " << grasp_datas_[arm_jmg]->ee_jmg_->getName());
    ee_traj->setRobotTrajectoryMsg(*current_state_, grasp_datas_[arm_jmg]->grasp_posture_); // closed
  }

  // Show the change in end effector
  if (verbose_)
  {
    visuals_->start_state_->publishRobotState(current_state_, rvt::GREEN);
    visuals_->goal_state_->publishRobotState(ee_traj->getLastWayPoint(), rvt::ORANGE);
  }

  // Check if already in new position
  if (statesEqual(*current_state_, ee_traj->getLastWayPoint(), ee_jmg))
  {
    ROS_INFO_STREAM_NAMED("manipulation","Not executing motion because current state and goal state are close enough.");
    return true;
  }

  // Convert trajectory to a message
  moveit_msgs::RobotTrajectory trajectory_msg;
  ee_traj->getRobotTrajectoryMsg(trajectory_msg);

  // Execute trajectory
  if( !executeTrajectory(trajectory_msg) )
  {
    ROS_ERROR_STREAM_NAMED("manipulation","Failed to execute grasp trajectory");
    return false;
  }

  return true;
}

bool Manipulation::openEndEffectorWithVelocity(bool open, const robot_model::JointModelGroup* arm_jmg)
{
  ROS_DEBUG_STREAM_NAMED("manipulation.superdebug","openEndEffectorWithVelocity()");

  getCurrentState();
  robot_trajectory::RobotTrajectoryPtr ee_trajectory(new robot_trajectory::RobotTrajectory(robot_model_, grasp_datas_[arm_jmg]->ee_jmg_));

  // Add goal state to trajectory
  if (open)
  {
    ROS_INFO_STREAM_NAMED("manipulation","Opening end effector for " << grasp_datas_[arm_jmg]->ee_jmg_->getName());
    ee_trajectory->setRobotTrajectoryMsg(*current_state_, grasp_datas_[arm_jmg]->pre_grasp_posture_); // open
  }
  else
  {
    ROS_INFO_STREAM_NAMED("manipulation","Closing end effector for " << grasp_datas_[arm_jmg]->ee_jmg_->getName());
    ee_trajectory->setRobotTrajectoryMsg(*current_state_, grasp_datas_[arm_jmg]->grasp_posture_); // closed
  }

  // Add start state to trajectory
  double dummy_dt = 1;
  ee_trajectory->addPrefixWayPoint(current_state_, dummy_dt);

  // Interpolate between each point
  double discretization = 0.1;
  interpolate(ee_trajectory, discretization);

  // Perform iterative parabolic smoothing
  double ee_velocity_scaling_factor = 0.1;
  iterative_smoother_.computeTimeStamps( *ee_trajectory, ee_velocity_scaling_factor );

  // Show the change in end effector
  if (verbose_)
  {
    visuals_->start_state_->publishRobotState(current_state_, rvt::GREEN);
    visuals_->goal_state_->publishRobotState(ee_trajectory->getLastWayPoint(), rvt::ORANGE);
  }

  // Check if already in new position
  if (statesEqual(*current_state_, ee_trajectory->getLastWayPoint(), grasp_datas_[arm_jmg]->ee_jmg_))
  {
    ROS_INFO_STREAM_NAMED("manipulation","Not executing motion because current state and goal state are close enough.");
    return true;
  }

  // Convert trajectory to a message
  moveit_msgs::RobotTrajectory trajectory_msg;
  ee_trajectory->getRobotTrajectoryMsg(trajectory_msg);

  // Visualize trajectory in Rviz display
  bool wait_for_trajetory = false;
  visuals_->visual_tools_->publishTrajectoryPath(trajectory_msg, current_state_, wait_for_trajetory);

  // Execute trajectory
  if( !executeTrajectory(trajectory_msg) )
  {
    ROS_ERROR_STREAM_NAMED("manipulation","Failed to execute grasp trajectory");
    return false;
  }

  return true;
}

bool Manipulation::setStateWithOpenEE(bool open, moveit::core::RobotStatePtr robot_state)
{
  ROS_DEBUG_STREAM_NAMED("manipulation.superdebug","setStateWithOpenEE()");

  if (open)
  {
    grasp_datas_[config_->right_arm_]->setRobotStatePreGrasp( robot_state );
    if (config_->dual_arm_)
      grasp_datas_[config_->left_arm_]->setRobotStatePreGrasp( robot_state );
  }
  else
  {
    grasp_datas_[config_->right_arm_]->setRobotStateGrasp( robot_state );
    if (config_->dual_arm_)
      grasp_datas_[config_->left_arm_]->setRobotStateGrasp( robot_state );
  }
}

bool Manipulation::checkExecutionManager()
{
  ROS_INFO_STREAM_NAMED("manipulation","Checking that execution manager is loaded.");

  trajectory_execution_manager_.reset(new trajectory_execution_manager::TrajectoryExecutionManager(robot_model_));
  plan_execution_.reset(new plan_execution::PlanExecution(planning_scene_monitor_, trajectory_execution_manager_));

  const robot_model::JointModelGroup* arm_jmg = config_->dual_arm_ ? config_->both_arms_ : config_->right_arm_;

  if (!trajectory_execution_manager_->ensureActiveControllersForGroup(arm_jmg->getName()))
  {
    ROS_ERROR_STREAM_NAMED("manipulation","Group " << arm_jmg->getName() << " does not have active controllers loaded");
    return false;
  }

  return true;
}

bool Manipulation::executeTrajectory(moveit_msgs::RobotTrajectory &trajectory_msg, bool ignore_collision)
{
  ROS_DEBUG_STREAM_NAMED("manipulation.superdebug","executeTrajectory()");
  ROS_INFO_STREAM_NAMED("manipulation","Executing trajectory with " << trajectory_msg.joint_trajectory.points.size() << " waypoints");

  // Hack? Remove acceleration command
  if (false)
    for (std::size_t i = 0; i < trajectory_msg.joint_trajectory.points.size(); ++i)
    {
      trajectory_msg.joint_trajectory.points[i].accelerations.clear();
    }

  // Debug
  ROS_DEBUG_STREAM_NAMED("manipulation.trajectory","Publishing:\n" << trajectory_msg);

  // Save to file
  if (true)
  {
    // Only save non-finger trajectories
    if (trajectory_msg.joint_trajectory.joint_names.size() > 3)
    {
      static std::size_t trajectory_count = 0;
      //saveTrajectory(trajectory_msg, "trajectory_"+ boost::lexical_cast<std::string>(trajectory_count++));
      saveTrajectory(trajectory_msg, "trajectory");
    }
  }

  // Create trajectory execution manager
  if( !trajectory_execution_manager_ )
  {
    trajectory_execution_manager_.reset(new trajectory_execution_manager::TrajectoryExecutionManager(robot_model_));
    plan_execution_.reset(new plan_execution::PlanExecution(planning_scene_monitor_, trajectory_execution_manager_));
  }

  // Confirm trajectory before continuing
  if (!remote_control_->getAutonomous() &&
      // Only wait for non-finger trajectories
      trajectory_msg.joint_trajectory.joint_names.size() > 3)
  {
    std::cout << std::endl;
    std::cout << std::endl;
    std::cout << "Waiting before executing trajectory" << std::endl;
    remote_control_->waitForNextStep();
  }

  ROS_INFO_STREAM_NAMED("manipulation","Executing trajectory...");

  // Clear
  plan_execution_->getTrajectoryExecutionManager()->clear();

  if(plan_execution_->getTrajectoryExecutionManager()->push(trajectory_msg))
  {
    plan_execution_->getTrajectoryExecutionManager()->execute();

    bool wait_exection = true;
    if (wait_exection)
    {
      // wait for the trajectory to complete
      moveit_controller_manager::ExecutionStatus es = plan_execution_->getTrajectoryExecutionManager()->waitForExecution();
      if (es == moveit_controller_manager::ExecutionStatus::SUCCEEDED)
      {
        ROS_DEBUG_STREAM_NAMED("manipulation","Trajectory execution succeeded");
      }
      else // Failed
      {
        if (es == moveit_controller_manager::ExecutionStatus::PREEMPTED)
          ROS_INFO_STREAM_NAMED("manipulation","Trajectory execution preempted");
        else
          if (es == moveit_controller_manager::ExecutionStatus::TIMED_OUT)
            ROS_ERROR_STREAM_NAMED("manipulation","Trajectory execution timed out");
          else
            ROS_ERROR_STREAM_NAMED("manipulation","Trajectory execution control failed");

        // Disable autonomous mode because something went wrong
        remote_control_->setAutonomous(false);

        return false;
      }
    }
  }
  else
  {
    ROS_ERROR_STREAM_NAMED("manipulation","Failed to execute trajectory");
    return false;
  }

  return true;
}

bool Manipulation::fixCollidingState(planning_scene::PlanningScenePtr cloned_scene)
{
  ROS_DEBUG_STREAM_NAMED("manipulation.superdebug","fixCollidingState()");

  // Turn off auto mode
  remote_control_->setAutonomous(false);

  const robot_model::JointModelGroup* arm_jmg = config_->dual_arm_ ? config_->left_arm_ : config_->right_arm_;

  // Decide what direction is needed to fix colliding state, using the cloned scene
  collision_detection::CollisionResult::ContactMap contacts;
  cloned_scene->getCollidingPairs(contacts);

  std::string colliding_world_object = "";
  for (collision_detection::CollisionResult::ContactMap::const_iterator contact_it = contacts.begin(); 
       contact_it != contacts.end(); contact_it++)
  {
    const std::string& body_id_1 = contact_it->first.first;
    const std::string& body_id_2 = contact_it->first.second;
    std::cout << "body_id_1: " << body_id_1 << std::endl;
    std::cout << "body_id_2: " << body_id_2 << std::endl;

    const std::vector<collision_detection::Contact>& contacts = contact_it->second;

    for (std::size_t i = 0; i < contacts.size(); ++i)
    {
      std::cout << "loop i= " << i << std::endl;
      const collision_detection::Contact& contact = contacts[i];

      // Find the world object that is the problem
      if (contact.body_type_1 == collision_detection::BodyTypes::WORLD_OBJECT)
      {
        colliding_world_object = contact.body_name_1;
        break;
      }
      if (contact.body_type_2 == collision_detection::BodyTypes::WORLD_OBJECT)
      {
        colliding_world_object = contact.body_name_2;
        break;
      }
    }
    if (!colliding_world_object.empty())
      break;
  }

  if (colliding_world_object.empty())
  {
    ROS_WARN_STREAM_NAMED("manipulation","Did not find any world objects in collision. Attempting to move home");
    bool check_validity = false;
    ROS_WARN_STREAM_NAMED("manipulation","DISABLED MOVE TO START POSITION TEMP");
    return false; //parent_->moveToStartPosition(NULL, check_validity);
  }

  ROS_INFO_STREAM_NAMED("manipulation","World object " << colliding_world_object << " in collision");

  // Categorize this world object
  bool reverse_out = false;
  bool raise_up = false;
  bool move_in_right = false;
  bool move_in_left = false;
  std::cout << "substring is: " << colliding_world_object.substr(0, 7) << std::endl;

  // if shelf or product, reverse out
  if (colliding_world_object.substr(0, 7) == "product")
  {
    reverse_out = true;
  }
  else if (colliding_world_object.substr(0, 7) == "front_w") // front_wall
  {
    reverse_out = true;
  }
  else if (colliding_world_object.substr(0, 7) == "shelf") // TODO string name
  {
    reverse_out = true;
  }
  // if goal bin, raise up
  else if (colliding_world_object.substr(0, 7) == "goal_bin") // TODO string name
  {
    raise_up = true;
  }
  // Right wall
  else if (colliding_world_object.substr(0, 7) == "right_w")
  {
    move_in_left = true;
  }
  // Left wall
  else if (colliding_world_object.substr(0, 7) == "left_w")
  {
    move_in_right = true;
  }
  else
  {
    int mode = iRand(0,3);
    ROS_WARN_STREAM_NAMED("manipulation","Unknown object, not sure how to handle. Performing random action using mode " << mode);
    
    if (mode == 0)
      reverse_out = true;
    else if (mode == 1)
      raise_up = true;
    else if (mode == 2)
      move_in_left = true;
    else // mode = 3
      move_in_right = true;
  }

  if (raise_up)
  {
    ROS_INFO_STREAM_NAMED("manipulation","Raising up");
    double desired_distance = 0.2;
    bool up = true;
    bool ignore_collision = true;
    if (!executeVerticlePath(arm_jmg, desired_distance, up, ignore_collision))
    {
      return false;
    }
  }
  else if (reverse_out)
  {
    ROS_INFO_STREAM_NAMED("manipulation","Reversing out");
    double desired_distance = 0.2;
    bool restreat = true;
    bool ignore_collision = true;
    if (!executeRetreatPath(arm_jmg, desired_distance, restreat, ignore_collision))
    {
      return false;
    }
  }
  else if (move_in_left)
  {
    ROS_INFO_STREAM_NAMED("manipulation","Moving in left");
    double desired_distance = 0.2;
    bool left = true;
    bool ignore_collision = true;
    if (!executeHorizontalPath(arm_jmg, desired_distance, left, ignore_collision))
    {
      return false;
    }
  }
  else if (move_in_right)
  {
    ROS_INFO_STREAM_NAMED("manipulation","Moving in right");
    double desired_distance = 0.2;
    bool left = false;
    bool ignore_collision = true;
    if (!executeHorizontalPath(arm_jmg, desired_distance, left, ignore_collision))
    {
      return false;
    }
  }


  return true;
}

bool Manipulation::saveTrajectory(const moveit_msgs::RobotTrajectory &trajectory_msg, const std::string &file_name)
{
  ROS_DEBUG_STREAM_NAMED("manipulation.superdebug","saveTrajectory()");

  const trajectory_msgs::JointTrajectory &joint_trajectory = trajectory_msg.joint_trajectory;

  // Error check
  if (!joint_trajectory.points.size() || !joint_trajectory.points[0].positions.size())
  {
    ROS_ERROR_STREAM_NAMED("manipulation","No trajectory points available to save");
    return false;
  }
  bool has_accelerations = true;
  if (joint_trajectory.points[0].accelerations.size() == 0)
  {
    has_accelerations = false;
  }

  std::string file_path;
  getFilePath(file_path, file_name);
  std::ofstream output_file;
  output_file.open (file_path.c_str());

  // Output header -------------------------------------------------------
  output_file << "time_from_start,";
  for (std::size_t j = 0; j < joint_trajectory.joint_names.size(); ++j)
  {
    output_file << joint_trajectory.joint_names[j] << "_pos,"
                << joint_trajectory.joint_names[j] << "_vel,";
    if (has_accelerations)
      output_file << joint_trajectory.joint_names[j] << "_acc,";
  }
  output_file << std::endl;

  // Output data ------------------------------------------------------

  // Subtract starting time

  for (std::size_t i = 0; i < joint_trajectory.points.size(); ++i)
  {
    // Timestamp
    output_file.precision(20);
    output_file << joint_trajectory.points[i].time_from_start.toSec() << ",";
    output_file.precision(5);
    // Output entire trajectory to single line
    for (std::size_t j = 0; j < joint_trajectory.points[i].positions.size(); ++j)
    {
      // Output State
      output_file << joint_trajectory.points[i].positions[j] << ","
                  << joint_trajectory.points[i].velocities[j] << ",";
      if (has_accelerations)
        output_file << joint_trajectory.points[i].accelerations[j] << ",";

    }

    output_file << std::endl;
  }
  output_file.close();
  ROS_DEBUG_STREAM_NAMED("manipulation","Saved trajectory to file " << file_path);
  return true;
}

bool Manipulation::allowFingerTouch(const std::string& object_name, const robot_model::JointModelGroup* arm_jmg)
{
  ROS_DEBUG_STREAM_NAMED("manipulation.superdebug","allowFingerTouch()");

  // TODO does this reset properly i.e. clear the matrix?

  // Lock planning scene
  {
    planning_scene_monitor::LockedPlanningSceneRW scene(planning_scene_monitor_); // Lock planning scene

    // Get links of end effector
    const std::vector<std::string> &ee_link_names = grasp_datas_[arm_jmg]->ee_jmg_->getLinkModelNames();

    // Prevent fingers from causing collision with object
    for (std::size_t i = 0; i < ee_link_names.size(); ++i)
    {
      ROS_DEBUG_STREAM_NAMED("manipulation.collision_matrix","Prevent collision between " << object_name << " and " << ee_link_names[i]);
      scene->getAllowedCollisionMatrixNonConst().setEntry(object_name, ee_link_names[i], true);
    }

    // Prevent object from causing collision with shelf
    for (std::size_t i = 0; i < shelf_->getShelfParts().size(); ++i)
    {
      ROS_DEBUG_STREAM_NAMED("manipulation.collision_matrix","Prevent collision between " << object_name << " and " << shelf_->getShelfParts()[i].getName());
      scene->getAllowedCollisionMatrixNonConst().setEntry(object_name, shelf_->getShelfParts()[i].getName(), true);
    }
  } // end lock planning scene

  // Debug current matrix
  if (false)
  {
    moveit_msgs::AllowedCollisionMatrix msg;
    planning_scene_monitor_->getPlanningScene()->getAllowedCollisionMatrix().getMessage(msg);
    std::cout << "Current collision matrix: " << msg << std::endl;
  }

  return true;
}

void Manipulation::loadPlanningPipeline()
{
  ROS_DEBUG_STREAM_NAMED("manipulation.superdebug","loadPlanningPipeline()");

  if (!planning_pipeline_)
  {
    // Setup planning pipeline
    planning_pipeline_.reset(new planning_pipeline::PlanningPipeline(robot_model_, nh_, "planning_plugin", "request_adapters"));
  }
}

bool Manipulation::statusPublisher(const std::string &status)
{
  ROS_DEBUG_STREAM_NAMED("manipulation.superdebug","statusPublisher()");

  std::cout << std::endl << std::endl;
  ROS_INFO_STREAM_NAMED("manipulation.status", status << " -------------------------------------");
  visuals_->visual_tools_->publishText(status_position_, status, rvt::WHITE, rvt::LARGE);
}

bool Manipulation::orderPublisher(WorkOrder& order)
{
  ROS_DEBUG_STREAM_NAMED("manipulation.superdebug","orderPublisher()");

  const std::string status = order.bin_->getName() + ": " + order.product_->getName();
  visuals_->visual_tools_->publishText(status_position_, status, rvt::WHITE, rvt::LARGE);
}

bool Manipulation::statesEqual(const moveit::core::RobotState &s1, const moveit::core::RobotState &s2,
                               const robot_model::JointModelGroup* arm_jmg)
{
  ROS_DEBUG_STREAM_NAMED("manipulation.superdebug","statesEqual()");
  static const double STATES_EQUAL_THRESHOLD = 0.01;

  double s1_vars[arm_jmg->getActiveJointModels().size()];
  double s2_vars[arm_jmg->getActiveJointModels().size()];
  s1.copyJointGroupPositions(arm_jmg, s1_vars);
  s2.copyJointGroupPositions(arm_jmg, s2_vars);

  for (std::size_t i = 0; i < arm_jmg->getActiveJointModels().size(); ++i)
  {
    //std::cout << "Diff of " << i << " - " << fabs(s1_vars[i] - s2_vars[i]) << std::endl;
    if ( fabs(s1_vars[i] - s2_vars[i]) > STATES_EQUAL_THRESHOLD )
    {
      return false;
    }
  }

  return true;
}

void Manipulation::displayLightningPlans(ompl::tools::ExperienceSetupPtr experience_setup,
                                         const robot_model::JointModelGroup* arm_jmg)
{
  ROS_DEBUG_STREAM_NAMED("manipulation.superdebug","displayLightningPlans()");

  // Create a state space describing our robot's planning group
  moveit_ompl::ModelBasedStateSpacePtr model_state_space
    = boost::dynamic_pointer_cast<moveit_ompl::ModelBasedStateSpace>(experience_setup->getStateSpace());

  //ROS_DEBUG_STREAM_NAMED("manipulation","Model Based State Space has dimensions: " << model_state_space->getDimension());

  // Load lightning and its database
  ompl::tools::LightningPtr lightning = boost::dynamic_pointer_cast<ompl::tools::Lightning>(experience_setup);
  //7lightning.setFile(arm_jmg->getName());

  // Get all of the paths in the database
  std::vector<ompl::base::PlannerDataPtr> paths;
  lightning->getAllPlannerDatas(paths);

  ROS_INFO_STREAM_NAMED("manipulation","Number of paths to publish: " << paths.size());

  // Load the OMPL visualizer
  if (!ompl_visual_tools_)
  {
    ompl_visual_tools_.reset(new ovt::OmplVisualTools(robot_model_->getModelFrame(),
                                                      "/ompl_experience_database", planning_scene_monitor_));
    ompl_visual_tools_->loadRobotStatePub("/picknik_amazon");
  }
  ompl_visual_tools_->deleteAllMarkers(); // clear all old markers
  ompl_visual_tools_->setStateSpace(model_state_space);

  // Get tip links for this setup
  std::vector<const robot_model::LinkModel*> tips;
  arm_jmg->getEndEffectorTips(tips);
  ROS_INFO_STREAM_NAMED("manipulation","Found " << tips.size() << " tips");

  bool show_trajectory_animated = false;//verbose_;

  // Loop through each path
  for (std::size_t path_id = 0; path_id < paths.size(); ++path_id)
  {
    std::cout << "Processing path " << path_id << std::endl;
    ompl_visual_tools_->publishRobotPath(paths[path_id], arm_jmg, tips, show_trajectory_animated);
  }

}

bool Manipulation::visualizeGrasps(std::vector<moveit_grasps::GraspCandidatePtr> grasp_candidates,
                                   const moveit::core::JointModelGroup *arm_jmg, bool show_cartesian_path)
{
  ROS_DEBUG_STREAM_NAMED("manipulation.superdebug","visualizeGrasps()");
  ROS_INFO_STREAM_NAMED("manipulation","Showing " << grasp_candidates.size() << " valid filtered grasp poses");

  // Publish in batch
  //visuals_->visual_tools_->enableBatchPublishing(true);

  // Get the-grasp
  moveit::core::RobotStatePtr the_grasp_state(new moveit::core::RobotState(*current_state_));

  Eigen::Vector3d approach_direction;
  approach_direction << -1, 0, 0; // backwards towards robot body
  double desired_approach_distance = 0.45; //0.12; //0.15;
  std::vector<moveit::core::RobotStatePtr> robot_state_trajectory;
  double path_length;
  double max_path_length = 0; // statistics
  bool reverse_path = false;

  for (std::size_t i = 0; i < grasp_candidates.size(); ++i)
  {
    if (!ros::ok())
      return false;

    if (show_cartesian_path)
    {
      the_grasp_state->setJointGroupPositions(arm_jmg, grasp_candidates[i]->grasp_ik_solution_);

      if (!computeStraightLinePath(approach_direction, desired_approach_distance,
                                   robot_state_trajectory, the_grasp_state, arm_jmg, reverse_path, path_length))
      {
        ROS_WARN_STREAM_NAMED("manipulation","Unable to find straight line path");
      }

      // Statistics
      if (path_length > max_path_length)
        max_path_length = path_length;

      bool blocking = false;
      double speed = 0.01;
      visuals_->visual_tools_->publishTrajectoryPath(robot_state_trajectory, arm_jmg, speed, blocking);
    }
    std::cout << "grasp_candidates[i]->grasp_.grasp_pose: " << grasp_candidates[i]->grasp_.grasp_pose << std::endl;
    visuals_->visual_tools_->publishZArrow(grasp_candidates[i]->grasp_.grasp_pose, rvt::RED);
    //grasp_generator_->publishGraspArrow(grasp_candidates[i]->grasp_.grasp_pose.pose, grasp_datas_[arm_jmg],
    //                                              rvt::BLUE, path_length);


    bool show_score = false;
    if (show_score)
    {
      const geometry_msgs::Pose& pose = grasp_candidates[i]->grasp_.grasp_pose.pose;
      double roll = atan2(2*(pose.orientation.x*pose.orientation.y + pose.orientation.w*pose.orientation.z), pose.orientation.w*pose.orientation.w + pose.orientation.x*pose.orientation.x - pose.orientation.y*pose.orientation.y - pose.orientation.z*pose.orientation.z);
      double yall = asin(-2*(pose.orientation.x*pose.orientation.z - pose.orientation.w*pose.orientation.y));
      double pitch = atan2(2*(pose.orientation.y*pose.orientation.z + pose.orientation.w*pose.orientation.x), pose.orientation.w*pose.orientation.w - pose.orientation.x*pose.orientation.x - pose.orientation.y*pose.orientation.y + pose.orientation.z*pose.orientation.z);
      std::cout << "ROLL: " << roll << " YALL: " << yall << " PITCH: " << pitch << std::endl;

      visuals_->visual_tools_->publishText(pose, boost::lexical_cast<std::string>(yall), rvt::BLACK, rvt::SMALL, false);
      //visuals_->visual_tools_->publishAxis(pose);
    }

  }
  //visuals_->visual_tools_->triggerBatchPublishAndDisable();

  ROS_INFO_STREAM_NAMED("learning","Maximum path length in approach trajetory was " << max_path_length);

  return true;
}

moveit::core::RobotStatePtr Manipulation::getCurrentState()
{
  ROS_DEBUG_STREAM_NAMED("manipulation.superdebug","getCurrentState()");
  planning_scene_monitor::LockedPlanningSceneRO scene(planning_scene_monitor_); // Lock planning scene
  (*current_state_) = scene->getCurrentState();
  return current_state_;
}

bool Manipulation::waitForRobotToStop(const double& timeout)
{
  ROS_INFO_STREAM_NAMED("manipulation","Waiting for robot to stop moving");
  ros::Time when_to_stop = ros::Time::now() + ros::Duration(timeout);

  static const double UPDATE_RATE = 0.1; // how often to check if robot is stopped
  static const double POSITION_ERROR_THRESHOLD = 0.002;
  static const std::size_t REQUIRED_STABILITY_PASSES = 4; // how many times it must be within threshold in a row
  std::size_t stability_passes = 0;
  double error;
  // Get the current position
  moveit::core::RobotState previous_position = *getCurrentState(); // copy the memory
  moveit::core::RobotState current_position = previous_position;  // copy the memory

  while (ros::ok())
  {
    ros::Duration(UPDATE_RATE).sleep();
    ros::spinOnce();

    current_position = *getCurrentState(); // copy the memory

    // Check if all positions are near zero
    bool stopped = true;
    for (std::size_t i = 0; i < current_position.getVariableCount(); ++i)
    {
      error = fabs(previous_position.getVariablePositions()[i] - current_position.getVariablePositions()[i]);

      if (error > POSITION_ERROR_THRESHOLD)
      {
        ROS_DEBUG_STREAM_NAMED("manipulation","Robot is still moving with error " << error << " on variable " << i);
        stopped = false;
      }
    }
    if (stopped)
    {
      stability_passes++;
      //ROS_INFO_STREAM_NAMED("manipulation","On stability pass " << stability_passes);
    }
    else
    {
      // Reset the stability passes because this round didn't pass
      stability_passes = 0;
    }

    if (stability_passes > 2)
      return true;

    // Check timeout
    if (ros::Time::now() > when_to_stop)
    {
      ROS_WARN_STREAM_NAMED("manipulation","Timed out while waiting for robot to stop");
      return false;
    }

    // Copy newest positions
    previous_position = current_position; // copy the memory
  }

  return false;
}

bool Manipulation::fixCurrentCollisionAndBounds(const robot_model::JointModelGroup* arm_jmg)
{
  ROS_DEBUG_STREAM_NAMED("manipulation.superdebug","fixCurrentCollisionAndBounds()");

  bool result = true;

  // Copy planning scene that is locked
  planning_scene::PlanningScenePtr cloned_scene;
  {
    planning_scene_monitor::LockedPlanningSceneRO scene(planning_scene_monitor_);
    cloned_scene = planning_scene::PlanningScene::clone(scene);
    (*current_state_) = scene->getCurrentState();
  }

  // Check for collisions
  bool verbose = true;
  if (cloned_scene->isStateColliding(*current_state_, arm_jmg->getName(), verbose))
  {
    result = false;

    std::cout << std::endl;
    std::cout << "-------------------------------------------------------" << std::endl;
    ROS_WARN_STREAM_NAMED("manipulation","State is colliding, attempting to fix...");
    std::cout << "-------------------------------------------------------" << std::endl;

    // Show collisions
    visuals_->visual_tools_->publishContactPoints(*current_state_, cloned_scene.get());
    visuals_->visual_tools_->publishRobotState(current_state_, rvt::RED);

    // Attempt to fix collision state
    if (!fixCollidingState(cloned_scene))
    {
      ROS_ERROR_STREAM_NAMED("manipulation","Unable to fix colliding state");
    }
  }
  else
  {
    ROS_INFO_STREAM_NAMED("manipulation","State is not colliding");
  }

  // Check if satisfies bounds
  if (!current_state_->satisfiesBounds(arm_jmg, fix_state_bounds_.getMaxBoundsError()))
  {
    std::cout << std::endl;
    std::cout << "-------------------------------------------------------" << std::endl;
    ROS_WARN_STREAM_NAMED("manipulation","State does not satisfy bounds, attempting to fix...");
    std::cout << "-------------------------------------------------------" << std::endl;

    moveit::core::RobotStatePtr new_state(new moveit::core::RobotState(*current_state_));

    if (!fix_state_bounds_.fixBounds(*new_state, arm_jmg))
    {
      ROS_WARN_STREAM_NAMED("manipulation","Unable to fix state bounds or change not required");
    }
    else
    {
      std::cout << std::endl;
      std::cout << std::endl;
      std::cout << "Waiting before executing trajectory" << std::endl;
      remote_control_->waitForNextStep();

      // State was modified, send to robot
      if (!executeState(new_state, arm_jmg, config_->main_velocity_scaling_factor_))
      {
        ROS_ERROR_STREAM_NAMED("manipulation","Unable to exceute state bounds fix");
      }
      result = false;
    }
  }
  else
  {
    ROS_INFO_STREAM_NAMED("manipulation","State satisfies bounds");
  }
  return result;
}

bool Manipulation::checkCollisionAndBounds(const moveit::core::RobotStatePtr &start_state,
                                           const moveit::core::RobotStatePtr &goal_state,
                                           bool verbose)
{
  ROS_DEBUG_STREAM_NAMED("manipulation.superdebug","checkCollisionAndBounds()");

  bool result = true;

  // Check if satisfies bounds  --------------------------------------------------------

  // Start
  if (!start_state->satisfiesBounds(fix_state_bounds_.getMaxBoundsError()))
  {
    if (verbose)
      ROS_WARN_STREAM_NAMED("manipulation","Start state does not satisfy bounds");
    result = false;
  }

  // Goal
  if (goal_state && !goal_state->satisfiesBounds(fix_state_bounds_.getMaxBoundsError()))
  {
    if (verbose)
      ROS_WARN_STREAM_NAMED("manipulation","Goal state does not satisfy bounds");
    //std::cout << "bounds: " << robot_model_->getJointModel("jaco2_joint_6")->getVariableBoundsMsg()[0] << std::endl;
    result = false;
  }

  // Check for collisions --------------------------------------------------------
  const robot_model::JointModelGroup* arm_jmg = config_->dual_arm_ ? config_->both_arms_ : config_->right_arm_;

  // Get planning scene lock
  {
    planning_scene_monitor::LockedPlanningSceneRO scene(planning_scene_monitor_);
    // Start
    if (scene->isStateColliding(*start_state, arm_jmg->getName(), verbose))
    {
      if (verbose)
      {
        ROS_WARN_STREAM_NAMED("manipulation","Start state is colliding");
        // Show collisions
        visuals_->visual_tools_->publishContactPoints(*start_state, planning_scene_monitor_->getPlanningScene().get());
        visuals_->visual_tools_->publishRobotState(*start_state, rvt::RED);
      }
      result = false;
    }

    // Goal
    if (goal_state)
    {
      goal_state->update();

      if (scene->isStateColliding(*goal_state, arm_jmg->getName(), verbose))
      {
        if (verbose)
        {
          ROS_WARN_STREAM_NAMED("manipulation","Goal state is colliding");
          // Show collisions
          visuals_->visual_tools_->publishContactPoints(*goal_state, planning_scene_monitor_->getPlanningScene().get());
          visuals_->visual_tools_->publishRobotState(*goal_state, rvt::RED);
        }
        result = false;
      }
    }
  }

  return result;
}

bool Manipulation::getFilePath(std::string &file_path, const std::string &file_name) const

{
  ROS_DEBUG_STREAM_NAMED("manipulation.superdebug","getFilePath()");
  namespace fs = boost::filesystem;

  // Check that the directory exists, if not, create it
  fs::path path;
  /*
    if (!std::string(getenv("HOME")).empty())
    path = fs::path(getenv("HOME")); // Support Linux/Mac
    else if (!std::string(getenv("HOMEPATH")).empty())
    path = fs::path(getenv("HOMEPATH")); // Support Windows
    else
    {
    ROS_WARN("Unable to find a home path for this computer");
    path = fs::path("");
    }
  */
  path = fs::path(package_path_ + "/trajectories");

  boost::system::error_code returnedError;
  fs::create_directories( path, returnedError );

  if ( returnedError )
  {
    //did not successfully create directories
    ROS_ERROR_STREAM_NAMED("manipulation", "Unable to create directory " << path.string());
    return false;
  }

  //directories successfully created, append the group name as the file name
  path = path / fs::path(file_name + ".csv");
  file_path = path.string();

  //ROS_DEBUG_STREAM_NAMED("manipulation","Using full file path" << file_path);
  return true;
}

} // end namespace

namespace
{
bool isStateValid(const planning_scene::PlanningScene *planning_scene, bool verbose,
                  picknik_main::VisualsPtr visuals, moveit::core::RobotState *robot_state,
                  const moveit::core::JointModelGroup *group, const double *ik_solution)
{
  robot_state->setJointGroupPositions(group, ik_solution);
  robot_state->update();

  if (!planning_scene)
  {
    ROS_ERROR_STREAM_NAMED("manipulation","No planning scene provided");
    return false;
  }
  if (!planning_scene->isStateColliding(*robot_state, group->getName()))
    return true; // not in collision

  // Display more info about the collision
  if (verbose)
  {
    visuals->visual_tools_->publishRobotState(*robot_state, rvt::RED);
    planning_scene->isStateColliding(*robot_state, group->getName(), true);
    visuals->visual_tools_->publishContactPoints(*robot_state, planning_scene);
    ros::Duration(0.4).sleep();
  }
  return false;
}
}
