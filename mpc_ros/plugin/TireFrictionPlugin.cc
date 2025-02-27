/*
 * Copyright (C) 2015 Open Source Robotics Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
*/

#include <algorithm>
#include <string>

#include "gazebo/common/Assert.hh"
#include "gazebo/physics/physics.hh"
#include "gazebo/physics/ode/ODESurfaceParams.hh"
#include "gazebo/physics/ode/ODETypes.hh"
#include "gazebo/transport/transport.hh"
#include "TireFrictionPluginPrivate.hh"
#include "TireFrictionPlugin.hh"

using namespace gazebo;

GZ_REGISTER_MODEL_PLUGIN(TireFrictionPlugin)

/////////////////////////////////////////////////
TireFrictionPlugin::TireFrictionPlugin()
  : dataPtr(new TireFrictionPluginPrivate)
{
  this->dataPtr->newMsg = false;
  this->dataPtr->frictionStatic  = 1.1;
  this->dataPtr->frictionDynamic = 1.0;
  this->dataPtr->slipStatic  = 0.1;
  this->dataPtr->slipDynamic = 0.2;
  this->dataPtr->speedStatic = 1.0;
}

/////////////////////////////////////////////////
TireFrictionPlugin::~TireFrictionPlugin()
{
}

/////////////////////////////////////////////////
void TireFrictionPlugin::Load(physics::ModelPtr _model, sdf::ElementPtr _sdf)
{
  this->dataPtr->model = _model;
  GZ_ASSERT(_model, "TireFrictionPlugin _model pointer is NULL");

  this->dataPtr->world = this->dataPtr->model->GetWorld();
  GZ_ASSERT(this->dataPtr->world, "TireFrictionPlugin world pointer is NULL");

  this->dataPtr->physics = this->dataPtr->world->Physics();
  GZ_ASSERT(this->dataPtr->physics,
            "TireFrictionPlugin physics pointer is NULL");

  this->dataPtr->sdf = _sdf;
  GZ_ASSERT(_sdf, "TireFrictionPlugin _sdf pointer is NULL");

  if (_sdf->HasElement("link_name"))
  {
    std::string linkName = _sdf->Get<std::string>("link_name");
    this->dataPtr->link = this->dataPtr->model->GetLink(linkName);
  }
  else
  {
    // link_name not supplied, get first link from model
    this->dataPtr->link = this->dataPtr->model->GetLink();
  }
  GZ_ASSERT(this->dataPtr->link, "TireFrictionPlugin link pointer is NULL");

  if (_sdf->HasElement("collision_name"))
  {
    std::string collisionName = _sdf->Get<std::string>("collision_name");
    this->dataPtr->collision = this->dataPtr->link->GetCollision(collisionName);
  }
  GZ_ASSERT(this->dataPtr->collision,
    "TireFrictionPlugin collision pointer is NULL");

  // Get tire friction parameters
  if (_sdf->HasElement("friction_static"))
    this->dataPtr->frictionStatic = _sdf->Get<double>("friction_static");
  if (_sdf->HasElement("friction_dynamic"))
    this->dataPtr->frictionDynamic = _sdf->Get<double>("friction_dynamic");

  if (_sdf->HasElement("slip_static"))
  {
    double valueCheck = _sdf->Get<double>("slip_static");
    if (valueCheck <= 0)
    {
      gzerr << "slip_static parameter value ["
            << valueCheck
            << "] must be positive, using default value ["
            << this->dataPtr->slipStatic
            << std::endl;
    }
    else
    {
      this->dataPtr->slipStatic = valueCheck;
    }
  }

  if (_sdf->HasElement("slip_dynamic"))
  {
    this->dataPtr->slipDynamic = _sdf->Get<double>("slip_dynamic");
  }
  if (this->dataPtr->slipDynamic <= this->dataPtr->slipStatic)
  {
    gzerr << "slip_dynamic parameter value ["
          << this->dataPtr->slipDynamic
          << "] must be greater than slip_static ["
          << this->dataPtr->slipStatic
          << "], using slip_static + 0.1 ["
          << this->dataPtr->slipStatic + 0.1
          << std::endl;
    this->dataPtr->slipDynamic = this->dataPtr->slipStatic + 0.1;
  }

  if (_sdf->HasElement("speed_static"))
  {
    double valueCheck = _sdf->Get<double>("speed_static");
    if (valueCheck <= 0)
    {
      gzerr << "speed_static parameter value ["
            << valueCheck
            << "] must be positive, using default value ["
            << this->dataPtr->speedStatic
            << std::endl;
    }
    else
    {
      this->dataPtr->speedStatic = valueCheck;
    }
  }
}

/////////////////////////////////////////////////
void TireFrictionPlugin::Init()
{
  this->dataPtr->node.reset(new transport::Node());
  this->dataPtr->node->Init(this->dataPtr->world->GetName());

  std::string topic =
    this->dataPtr->physics->GetContactManager()->CreateFilter(
      this->dataPtr->collision->GetScopedName(),
      this->dataPtr->collision->GetScopedName());

  // Subscribe to the contact topic
  this->dataPtr->contactSub = this->dataPtr->node->Subscribe(topic,
    &TireFrictionPlugin::OnContacts, this);

  this->dataPtr->updateConnection = event::Events::ConnectWorldUpdateBegin(
          boost::bind(&TireFrictionPlugin::OnUpdate, this));
}

/////////////////////////////////////////////////
void TireFrictionPlugin::OnContacts(ConstContactsPtr &_msg)
{
  boost::mutex::scoped_lock lock(this->dataPtr->mutex);
  this->dataPtr->newestContactsMsg = *_msg;
  this->dataPtr->newMsg = true;
}

/////////////////////////////////////////////////
void TireFrictionPlugin::OnUpdate()
{
  // Only update when there is a new contact message.
  if (!this->dataPtr->newMsg)
  {
    // Use time step to track wait time between messages.
    double dt = this->dataPtr->physics->GetMaxStepSize();
    this->dataPtr->newMsgWait += common::Time(dt);

    const common::Time messageTime(1, 0);
    if (this->dataPtr->newMsgWait > messageTime)
    {
      gzlog << "Waited "
            << this->dataPtr->newMsgWait.Double()
            << " s without a contact message"
            << std::endl;
      this->dataPtr->newMsgWait.Set(0, 0);
    }
    return;
  }

  // Copy contacts message so that mutex lock is short.
  msgs::Contacts contacts;
  {
    boost::mutex::scoped_lock lock(this->dataPtr->mutex);
    contacts = this->dataPtr->newestContactsMsg;
    this->dataPtr->newMsg = false;
  }
  this->dataPtr->newMsgWait.Set(0, 0);

  // Compute slip at contact points.
  // For each contact point:
  // Compute slip velocity
  // * compute relative velocity between bodies at contact point
  // * subtract velocity component parallel to normal vector
  // * take sum of velocities, weighted by normal force
  // Compute reference velocity
  // * max velocity magnitude
  double scaledFriction = 0.0;
  double contactsNormalForceSum = 0.0;
  for (int i = 0; i < contacts.contact_size(); ++i)
  {
    // Get pointers to collision objects
    const msgs::Contact *contact = &contacts.contact(i);
    const std::string collision1(contact->collision1());
    const std::string collision2(contact->collision2());
    physics::CollisionPtr collPtr1 =
      boost::dynamic_pointer_cast<physics::Collision>(
      this->dataPtr->world->GetEntity(collision1));
    physics::CollisionPtr collPtr2 =
      boost::dynamic_pointer_cast<physics::Collision>(
      this->dataPtr->world->GetEntity(collision2));
    physics::LinkPtr link1 = collPtr1->GetLink();
    physics::LinkPtr link2 = collPtr2->GetLink();

    // compute velocity at each contact point
    if (contact->position_size() == 0 ||
        contact->position_size() != contact->normal_size() ||
        contact->position_size() != contact->wrench_size())
    {
      gzerr << "No contacts or invalid contact message"
            << std::endl;
      continue;
    }

    double scaledSlipSpeed = 0.0;
    double scaledReferenceSpeed = 0.0;
    double contactNormalForceSum = 0.0;
    for (int j = 0; j < contact->position_size(); ++j)
    {
      // Contact position in world coordinates.
      auto position = msgs::ConvertIgn(contact->position(j));

      // Velocity of each link at contact point in world coordinates.
      ignition::math::Vector3d velocity1;
      ignition::math::Vector3d velocity2;
      {
        ignition::math::Pose3d linkPose = link1->GetWorldPose().Ign();
        ignition::math::Vector3d offset = position - linkPose.Pos();
        velocity1 = link1->GetWorldLinearVel(offset, math::Quaternion()).Ign();
      }
      {
        ignition::math::Pose3d linkPose = link2->GetWorldPose().Ign();
        ignition::math::Vector3d offset = position - linkPose.Pos();
        velocity2 = link2->GetWorldLinearVel(offset, math::Quaternion()).Ign();
      }

      // Relative link velocity at contact point.
      ignition::math::Vector3d slipVelocity = velocity1 - velocity2;

      // Subtract normal velocity component
      auto normal = msgs::ConvertIgn(contact->normal(j));
      slipVelocity -= normal * slipVelocity.Dot(normal);

      // Scale slip speed by normal force
      double slipSpeed = slipVelocity.Length();
      double normalForce;
      {
        ignition::math::Vector3d force1 = msgs::ConvertIgn(
          contact->wrench(j).body_1_wrench().force());
        ignition::math::Quaterniond rot1 = link1->GetWorldPose().Ign().Rot();
        normalForce = rot1.RotateVector(force1).Dot(normal);
      }
      scaledSlipSpeed += slipSpeed * std::abs(normalForce);
      contactNormalForceSum += std::abs(normalForce);

      // Compute reference speed
      // max of absolute speed at contact points and at link origin
      double referenceSpeed =
        std::max(velocity1.Length(), velocity2.Length());
      referenceSpeed = std::max(referenceSpeed,
        link1->GetWorldLinearVel().Ign().Length());
      referenceSpeed = std::max(referenceSpeed,
        link2->GetWorldLinearVel().Ign().Length());
      scaledReferenceSpeed += referenceSpeed * std::abs(normalForce);
    }

    // Compute aggregate slip and reference speed (m/s)
    double slipSpeed = scaledSlipSpeed / contactNormalForceSum;
    double referenceSpeed = scaledReferenceSpeed / contactNormalForceSum;

    // Compute friction as a function of slip and reference speeds.
    double friction = this->ComputeFriction(slipSpeed, referenceSpeed);
    scaledFriction += friction * contactNormalForceSum;

    contactsNormalForceSum += contactNormalForceSum;
  }
  double friction = scaledFriction / contactsNormalForceSum;

  // Set friction coefficient.
  if (this->dataPtr->physics->GetType() == "ode")
  {
    physics::SurfaceParamsPtr surface =
        this->dataPtr->collision->GetSurface();
    if (surface)
    {
      // ideally we should change fdir1 I think?
      surface->FrictionPyramid()->SetMuPrimary(friction);
      surface->FrictionPyramid()->SetMuSecondary(friction);
    }
    else
    {
      gzerr << "Setting friction failed" << std::endl;
    }
  }
  else
  {
    gzerr << "Only ODE is supported right now" << std::endl;
  }
}

/////////////////////////////////////////////////////////////////////
// This is an example function for computing friction based on slip
// and reference speed.
//
// The model has five parameters:
// * frictionStatic
// * frictionDynamic
// * slipStatic
// * slipDynamic
// * speedStatic
//
// The model behaves differently in three speed ranges:
//
// When the _referenceSpeed is high (larger than speedStatic parameter),
// slipRatio is computed as the ratio of _slipSpeed to _referenceSpeed.
// The tire friction coefficient is computed as a piecewise linear
// function of the slip ratio.
// A plot of this function is given below with the slipRatio on the
// horizontal axis, and friction on the vertical axis.
// The piecewise function connects the following points:
// * (0,0)
// * (slipStatic,frictionStatic)
// * (slipDynamic,frictionDynamic)
// * (Inf,frictionDynamic)
//
//   |                                            .
//   |         frictionStatic                     .
//   |        /.\                                 .
//   |       / . \                                .
//   |      /  .  \_____________ frictionDynamic  .
//   |     /   .  .                               .
//   |    /    .  .                               .
//   |   /     .  .                               .
//   |  /      .  .                               .
//   | /       .  .                               .
//   |/        .  .                               .
// --+-------------------------- slipRatio
//   |         |  └— slipDynamic
//   |         └— slipStatic
//
// This model is a piecewise linear approximation of the Pacejka
// magic formula and other semi-empirical tire models. These formulae
// require adjustments at low-speed, however.
//
// When the _referenceSpeed is low (below 50% of the speedStatic parameter),
// the frictionStatic parameter is always returned.
//
// To make the function continuous, the two value are interpolated
// when the _referenceSpeed lies between 50% and 100% of the speedStatic
// parameter.
/////////////////////////////////////////////////////////////////////
double TireFrictionPlugin::ComputeFriction(const double _slipSpeed,
                                           const double _referenceSpeed)
{
  // For very low speeds, there can be numerical problems.
  // Thus don't compute friction based on slip if
  // reference speed is less than 50% of static speed;
  // just use static friction coefficient.
  if (std::abs(_referenceSpeed) < 0.5 * std::abs(this->dataPtr->speedStatic))
  {
    return this->dataPtr->frictionStatic;
  }

  // Compute slip ratio:
  double slipRatio = std::abs(_slipSpeed) / std::abs(_referenceSpeed);

  // Compute friction as function of slip:
  const double muStatic = std::abs(this->dataPtr->frictionStatic);
  const double muDynamic = std::abs(this->dataPtr->frictionDynamic);

  // note muDynamic value corresponds to slipRation >= slipDynamic,
  // so we only need two if statements to check other values
  double frictionFromSlip = muDynamic;
  if (slipRatio < this->dataPtr->slipStatic)
  {
    frictionFromSlip = slipRatio * muStatic / this->dataPtr->slipStatic;
  }
  else if (slipRatio < this->dataPtr->slipDynamic)
  {
    frictionFromSlip = muDynamic + (muStatic - muDynamic)
      / (this->dataPtr->slipStatic - this->dataPtr->slipDynamic)
      * (slipRatio - this->dataPtr->slipDynamic);
  }

  // Now that friction is computed from slip, do some additional smoothing
  // at moderate speeds (between 50% and 100% speedStatic)
  double speedRatio = std::abs(_referenceSpeed)
    / std::abs(this->dataPtr->speedStatic);
  if (speedRatio >= 0.5 && speedRatio < 1.0)
  {
    return (frictionFromSlip - this->dataPtr->frictionStatic)
      / 0.5 * (speedRatio - 0.5);
  }

  // Otherwise speeds are high enough, so return friction from slip.
  return frictionFromSlip;
}
