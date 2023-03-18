/***********************************************************
 *
 * @file: pedestrian_sfm_plugin.cpp
 * @breif: Gazebo plugin for pedestrians using social force model
 * @author: Yang Haodong
 * @update: 2023-03-15
 * @version: 1.1
 *
 * Copyright (c) 2023， Yang Haodong
 * All rights reserved.
 * --------------------------------------------------------
 *
 **********************************************************/
#include <functional>
#include <stdio.h>
#include <string>

#include <pedestrian_sfm_plugin.h>

#define WALKING_ANIMATION "walking"

using namespace gazebo;
GZ_REGISTER_MODEL_PLUGIN(PedestrianSFMPlugin)


/**
 * @brief Construct a gazebo plugin
 */
PedestrianSFMPlugin::PedestrianSFMPlugin() {}

/**
 * @brief Load the actor plugin.
 * @param _model  Pointer to the parent model.
 * @param _sdf    Pointer to the plugin's SDF elements.
 */
void PedestrianSFMPlugin::Load(physics::ModelPtr _model, sdf::ElementPtr _sdf) {
  // gazebo model pointer
  sdf_ = _sdf;
  actor_ = boost::dynamic_pointer_cast<physics::Actor>(_model);
  world_ = actor_->GetWorld();

  // Create the ROS node
  if (!ros::isInitialized())
  {
    int argc = 0;
    char **argv = NULL;
    ros::init(argc, argv, "gazebo_client", ros::init_options::NoSigintHandler);
  }
  node_.reset(new ros::NodeHandle("gazebo_client"));

  // topic publisher
  pose_pub_ = node_->advertise<geometry_msgs::PoseStamped>("/" + actor_->GetName() + "/pose", 10);

  // Collision attribute configuration of pedestrian links
  std::map<std::string, ignition::math::Vector3d> scales;
  std::map<std::string, ignition::math::Pose3d> offsets;

  // Read in the collision property
  if (sdf_->HasElement("collision"))
  {
    auto elem = sdf_->GetElement("collision");
    while (elem)
    {
      auto name = elem->Get<std::string>("collision");
      if (elem->HasAttribute("scale"))
      {
        auto scale = elem->Get<ignition::math::Vector3d>("scale");
        scales[name] = scale;
      }
      if (elem->HasAttribute("pose"))
      {
        auto pose = elem->Get<ignition::math::Pose3d>("pose");
        offsets[name] = pose;
      }
      elem = elem->GetNextElement("collision");
    }
  }
  // Configure the links
  for (const auto &link : actor_->GetLinks())
  {
    // Init the links, which in turn enables collisions
    link->Init();

    if (scales.empty())
      continue;
 
    // Process all the collisions in all the links
    for (const auto &collision : link->GetCollisions())
    {
      auto name = collision->GetName();
      if (scales.find(name) != scales.end())
      {
        auto boxShape = boost::dynamic_pointer_cast<gazebo::physics::BoxShape>(collision->GetShape());
        // Make sure we have a box shape.
        if (boxShape)
          boxShape->SetSize(boxShape->Size() * scales[name]);
      }

      if (offsets.find(name) != offsets.end())
      {
        collision->SetInitialRelativePose(offsets[name] + collision->InitialRelativePose());
      }
    }
  }

  // Bind the update callback function
  connections_.push_back(event::Events::ConnectWorldUpdateBegin(
      std::bind(&PedestrianSFMPlugin::OnUpdate, this, std::placeholders::_1)));

  // Initialize the social force model.
  this->Reset();
}


/**
 * @brief Initialize the social force model.
 */
void PedestrianSFMPlugin::Reset() {
  last_update_ = 0;
  sfm_actor_.id = actor_->GetId();

  // Read in the goals to reach
  if (sdf_->HasElement("cycle")) 
    sfm_actor_.cyclicGoals = sdf_->GetElement("cycle")->Get<bool>();
  
  if (sdf_->HasElement("trajectory")) 
  {
    sdf::ElementPtr model_elem =
        sdf_->GetElement("trajectory")->GetElement("goalpoint");
    while (model_elem) {
      ignition::math::Pose3d g = model_elem->Get<ignition::math::Pose3d>();
      sfm::Goal goal;
      goal.center.set(g.Pos().X(), g.Pos().Y());
      goal.radius = 0.3;
      sfm_actor_.goals.push_back(goal);
      model_elem = model_elem->GetNextElement("goalpoint");
    }
  }

  auto skel_anims = actor_->SkeletonAnimations();
  if (skel_anims.find(WALKING_ANIMATION) == skel_anims.end()) {
    gzerr << "Skeleton animation " << WALKING_ANIMATION << " not found.\n";
  } else {
    // Create custom trajectory
    trajectory_info_.reset(new physics::TrajectoryInfo());
    trajectory_info_->type = WALKING_ANIMATION;
    trajectory_info_->duration = 1.0;
    actor_->SetCustomTrajectory(trajectory_info_);
  }

  // Initialize sfm actor position
  ignition::math::Vector3d pos = actor_->WorldPose().Pos();
  ignition::math::Vector3d rpy = actor_->WorldPose().Rot().Euler();
  sfm_actor_.position.set(pos.X(), pos.Y());
  sfm_actor_.yaw = utils::Angle::fromRadian(rpy.Z());
  ignition::math::Vector3d linvel = actor_->WorldLinearVel();
  sfm_actor_.velocity.set(linvel.X(), linvel.Y());
  sfm_actor_.linearVelocity = linvel.Length();
  ignition::math::Vector3d angvel = actor_->WorldAngularVel();
  sfm_actor_.angularVelocity = angvel.Z();

  // Read in the maximum velocity of the pedestrian
  if (sdf_->HasElement("velocity"))
    sfm_actor_.desiredVelocity = sdf_->Get<double>("velocity");
  else
    sfm_actor_.desiredVelocity = 0.8;

  // Read in the algorithm weights
  if (sdf_->HasElement("goal_weight"))
    sfm_actor_.params.forceFactorDesired = sdf_->Get<double>("goal_weight");
  if (sdf_->HasElement("obstacle_weight"))
    sfm_actor_.params.forceFactorObstacle = sdf_->Get<double>("obstacle_weight");
  if (sdf_->HasElement("social_weight"))
    sfm_actor_.params.forceFactorSocial = sdf_->Get<double>("social_weight");
  if (sdf_->HasElement("group_gaze_weight"))
    sfm_actor_.params.forceFactorGroupGaze = sdf_->Get<double>("group_gaze_weight");
  if (sdf_->HasElement("group_coh_weight"))
    sfm_actor_.params.forceFactorGroupCoherence = sdf_->Get<double>("group_coh_weight");
  if (sdf_->HasElement("group_rep_weight"))
    sfm_actor_.params.forceFactorGroupRepulsion = sdf_->Get<double>("group_rep_weight");

  // Read in the animation factor (applied in the OnUpdate function).
  if (sdf_->HasElement("animation_factor"))
    animation_factor_ = sdf_->Get<double>("animation_factor");
  else
    animation_factor_ = 4.5;

  if (sdf_->HasElement("people_distance"))
    people_dist_ = sdf_->Get<double>("people_distance");
  else
    people_dist_ = 5.0;

  // Read in the pedestrians in your walking group
  if (sdf_->HasElement("group")) {
    sfm_actor_.groupId = sfm_actor_.id;
    sdf::ElementPtr model_elem = sdf_->GetElement("group")->GetElement("model");
    while (model_elem) {
      group_names_.push_back(model_elem->Get<std::string>());
      model_elem = model_elem->GetNextElement("model");
    }
    sfm_actor_.groupId = sfm_actor_.id;
  } else
    sfm_actor_.groupId = -1;

  // Read in the other obstacles to ignore
  if (sdf_->HasElement("ignore_obstacles")) {
    sdf::ElementPtr model_elem = sdf_->GetElement("ignore_obstacles")->GetElement("model");
    while (model_elem) {
      ignore_models_.push_back(model_elem->Get<std::string>());
      model_elem = model_elem->GetNextElement("model");
    }
  }
  // Add our own name to models we should ignore when avoiding obstacles.
  ignore_models_.push_back(actor_->GetName());

  // Add the other pedestrians to the ignored obstacles
  for (unsigned int i = 0; i < world_->ModelCount(); ++i) {
    physics::ModelPtr model = world_->ModelByIndex(i);

    if (model->GetId() != actor_->GetId() && ((int)model->GetType() == (int)actor_->GetType()))
      ignore_models_.push_back(model->GetName());
  }
}

/**
 * @brief Helper function to detect the closest obstacles.
 */
void PedestrianSFMPlugin::handleObstacles() {
  double min_dist = 10000.0;
  ignition::math::Vector3d closest_obs;
  sfm_actor_.obstacles1.clear();

  for (unsigned int i = 0; i < world_->ModelCount(); ++i) 
  {
    physics::ModelPtr model = world_->ModelByIndex(i);
    if (std::find(ignore_models_.begin(), ignore_models_.end(), model->GetName()) == ignore_models_.end())
    {
      ignition::math::Vector3d actorPos = actor_->WorldPose().Pos();
      ignition::math::Vector3d modelPos = model->WorldPose().Pos();
      std::tuple<bool, double, ignition::math::Vector3d> intersect =
          model->BoundingBox().Intersect(modelPos, actorPos, 0.05, 8.0);

      ignition::math::Vector3d offset = std::get<2>(intersect) - actorPos;
      double model_dist = offset.Length();

      if (model_dist < min_dist) {
        min_dist = model_dist;
        closest_obs = std::get<2>(intersect);
      }
    }
  }

  utils::Vector2d ob(closest_obs.X(), closest_obs.Y());
  sfm_actor_.obstacles1.push_back(ob);
}

/**
 * @brief Helper function to detect the nearby pedestrians (other actors).
 */
void PedestrianSFMPlugin::handlePedestrians() {
  other_actors_.clear();

  for (unsigned int i = 0; i < world_->ModelCount(); ++i) 
  {
    physics::ModelPtr model = world_->ModelByIndex(i);

    if (model->GetId() != actor_->GetId() && ((int)model->GetType() == (int)actor_->GetType()))
    {
      ignition::math::Pose3d model_pose = model->WorldPose();
      ignition::math::Vector3d pos = model_pose.Pos() - actor_->WorldPose().Pos();
      if (pos.Length() < people_dist_) 
      {
        sfm::Agent ped;
        ped.id = model->GetId();
        ped.position.set(model_pose.Pos().X(), model_pose.Pos().Y());
        ignition::math::Vector3d rpy = model_pose.Rot().Euler();
        ped.yaw = utils::Angle::fromRadian(rpy.Z());

        ped.radius = sfm_actor_.radius;
        ignition::math::Vector3d linvel = model->WorldLinearVel();
        ped.velocity.set(linvel.X(), linvel.Y());
        ped.linearVelocity = linvel.Length();
        ignition::math::Vector3d angvel = model->WorldAngularVel();
        ped.angularVelocity = angvel.Z();

        // check if the ped belongs to my group
        if (sfm_actor_.groupId != -1) {
          std::vector<std::string>::iterator it;
          it = find(group_names_.begin(), group_names_.end(), model->GetName());
          if (it != group_names_.end())
            ped.groupId = sfm_actor_.groupId;
          else
            ped.groupId = -1;
        }
        other_actors_.push_back(ped);
      }
    }
  }
}

/**
 * @brief Function that is called every update cycle.
 * @param _info Timing information.
 */
void PedestrianSFMPlugin::OnUpdate(const common::UpdateInfo &_info) {
  // Time delta
  double dt = (_info.simTime - last_update_).Double();

  ignition::math::Pose3d actor_pose = actor_->WorldPose();

  // update closest obstacle
  this->handleObstacles();
  // update pedestrian around
  this->handlePedestrians();

  // Compute Social Forces
  sfm::SFM.computeForces(sfm_actor_, other_actors_);
  // Update model
  sfm::SFM.updatePosition(sfm_actor_, dt);

 utils::Angle h = this->sfm_actor_.yaw;
  utils::Angle add = utils::Angle::fromRadian(1.5707);
  h = h + add;
  double yaw = h.toRadian();

  ignition::math::Vector3d rpy = actor_pose.Rot().Euler();
  utils::Angle current = utils::Angle::fromRadian(rpy.Z());
  double diff = (h- current).toRadian();
  if (std::fabs(diff) > IGN_DTOR(10)) {
    current = current + utils::Angle::fromRadian(diff * 0.005);
    yaw = current.toRadian();
  }

  actor_pose.Pos().X(sfm_actor_.position.getX());
  actor_pose.Pos().Y(sfm_actor_.position.getY());
  actor_pose.Pos().Z(1.0);
  actor_pose.Rot() = ignition::math::Quaterniond(1.5707, 0, yaw);

  // Distance traveled is used to coordinate motion with the walking
  double distance_traveled = (actor_pose.Pos() - actor_->WorldPose().Pos()).Length();

  actor_->SetWorldPose(actor_pose);
  actor_->SetScriptTime(actor_->ScriptTime() + distance_traveled * animation_factor_);
  
  geometry_msgs::PoseStamped current_pose;
  current_pose.header.frame_id = "map";
  current_pose.header.stamp = ros::Time::now();
  current_pose.pose.position.x = sfm_actor_.position.getX();
  current_pose.pose.position.y = sfm_actor_.position.getY();
  current_pose.pose.position.z = 1.0;
  tf2::Quaternion q;
  q.setRPY(0, 0, sfm_actor_.yaw.toRadian());
  tf2::convert(q, current_pose.pose.orientation);

  pose_pub_.publish(current_pose);

  last_update_ = _info.simTime;
}
