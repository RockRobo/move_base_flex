/*
 *  Copyright 2018, Sebastian Pütz
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *
 *  2. Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *
 *  3. Neither the name of the copyright holder nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 *  abstract_action.h
 *
 *  author: Sebastian Pütz <spuetz@uni-osnabrueck.de>
 *
 */

#ifndef MBF_ABSTRACT_NAV__ABSTRACT_ACTION_BASE_H_
#define MBF_ABSTRACT_NAV__ABSTRACT_ACTION_BASE_H_

#include <actionlib/server/action_server.h>
#include <mbf_abstract_nav/MoveBaseFlexConfig.h>
#include "mbf_abstract_nav/robot_information.h"

namespace mbf_abstract_nav{

template <typename Action, typename Execution>
class AbstractActionBase
{
 public:
  typedef boost::shared_ptr<AbstractActionBase> Ptr;
  typedef typename actionlib::ActionServer<Action>::GoalHandle GoalHandle;
  typedef boost::function<void (GoalHandle &goal_handle, Execution &execution)> RunMethod;
  typedef struct{
    typename Execution::Ptr execution;
    boost::thread* thread_ptr;
    GoalHandle goal_handle;
  } ConcurrencySlot;


  AbstractActionBase(
      const std::string& name,
      const RobotInformation &robot_info,
      const RunMethod run_method
  ) : name_(name), robot_info_(robot_info), run_(run_method){}

  virtual void start(
      GoalHandle &goal_handle,
      typename Execution::Ptr execution_ptr
  )
  {
    uint8_t slot = goal_handle.getGoal()->concurrency_slot;

    if(goal_handle.getGoalStatus().status == actionlib_msgs::GoalStatus::RECALLING)
    {
      goal_handle.setCanceled();
    }
    else {
      slot_map_mtx_.lock();
      typename std::map<uint8_t, ConcurrencySlot>::iterator slot_it =
          concurrency_slots_.find(slot);
      slot_map_mtx_.unlock();
      if (slot_it != concurrency_slots_.end()) {
        // if there is a plugin running on the same slot, cancel it
        slot_it->second.execution->cancel();
        if (slot_it->second.thread_ptr->joinable()) {
          slot_it->second.thread_ptr->join();
        }
      }
      boost::lock_guard<boost::mutex> guard(slot_map_mtx_);
      // fill concurrency slot with the new goal handle, execution, and working thread
      concurrency_slots_[slot].goal_handle = goal_handle;
      concurrency_slots_[slot].goal_handle.setAccepted();
      concurrency_slots_[slot].execution = execution_ptr;
      concurrency_slots_[slot].thread_ptr = threads_.create_thread(boost::bind(
          &AbstractActionBase::runAndCleanUp, this,
          boost::ref(concurrency_slots_[slot].goal_handle), execution_ptr));
    }
  }

  virtual void cancel(GoalHandle &goal_handle){
    uint8_t slot = goal_handle.getGoal()->concurrency_slot;

    boost::lock_guard<boost::mutex> guard(slot_map_mtx_);
    typename std::map<uint8_t, ConcurrencySlot>::iterator slot_it = concurrency_slots_.find(slot);
    if(slot_it != concurrency_slots_.end())
    {
      concurrency_slots_[slot].execution->cancel();
    }
  }

  virtual void runAndCleanUp(GoalHandle &goal_handle, typename Execution::Ptr execution_ptr){
    uint8_t slot = goal_handle.getGoal()->concurrency_slot;

    execution_ptr->preRun();
    run_(goal_handle, *execution_ptr);
    ROS_DEBUG_STREAM_NAMED(name_, "Finished action \"" << name_ << "\" run method, waiting for execution thread to finish.");
    execution_ptr->join();
    ROS_DEBUG_STREAM_NAMED(name_, "Execution thread for action \"" << name_ << "\" stopped, cleaning up execution leftovers.");
    boost::lock_guard<boost::mutex> guard(slot_map_mtx_);
    ROS_DEBUG_STREAM_NAMED(name_, "Exiting run method with goal status "
                           << (int)concurrency_slots_[slot].goal_handle.getGoalStatus().status
                           << ": "<< concurrency_slots_[slot].goal_handle.getGoalStatus().text);
    threads_.remove_thread(concurrency_slots_[slot].thread_ptr);
    delete concurrency_slots_[slot].thread_ptr;
    concurrency_slots_.erase(slot);
    execution_ptr->postRun();
  }

  virtual void reconfigureAll(
      mbf_abstract_nav::MoveBaseFlexConfig &config, uint32_t level)
  {
    boost::lock_guard<boost::mutex> guard(slot_map_mtx_);

    typename std::map<uint8_t, ConcurrencySlot>::iterator iter;
    for(iter = concurrency_slots_.begin(); iter != concurrency_slots_.end(); ++iter)
    {
      iter->second.execution->reconfigure(config);
    }
  }

  virtual void cancelAll()
  {
    ROS_INFO_STREAM_NAMED(name_, "Cancel all goals for \"" << name_ << "\".");
    boost::lock_guard<boost::mutex> guard(slot_map_mtx_);
    typename std::map<uint8_t, ConcurrencySlot>::iterator iter;
    for(iter = concurrency_slots_.begin(); iter != concurrency_slots_.end(); ++iter)
    {
      iter->second.execution->cancel();
    }
    threads_.join_all();
  }

protected:
  const std::string &name_;
  const RobotInformation &robot_info_;

  RunMethod run_;
  boost::thread_group threads_;
  std::map<uint8_t, ConcurrencySlot> concurrency_slots_;

  boost::mutex slot_map_mtx_;

};

}

#endif /* MBF_ABSTRACT_NAV__ABSTRACT_ACTION_BASE_H_ */
