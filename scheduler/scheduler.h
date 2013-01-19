/* -*- Mode: C++; c-file-style: "gnu"; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2013 University of California, Los Angeles
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Author: Zhenkai Zhu <zhenkai@cs.ucla.edu>
 *         Alexander Afanasyev <alexander.afanasyev@ucla.edu>
 */

#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <event2/event.h>
#include <event2/thread.h>
#include <event2/event-config.h>
#include <event2/util.h>

#include <boost/function.hpp>
#include <boost/shared_ptr.hpp>

#include <boost/exception/all.hpp>
#include <boost/thread/shared_mutex.hpp>
#include <boost/thread/thread.hpp>
#include <math.h>
#include <map>
#include <sys/time.h>

#include "task.h"

/**
 * @brief Scheduler class
 */
class Scheduler
{
public:
  Scheduler();
  virtual ~Scheduler();

  // start event scheduling
  virtual void
  start();

  // stop event scheduling
  virtual void
  shutdown();

  // if task with the same tag exists, the task is not added and return false
  virtual bool
  addTask(const TaskPtr &task);

  // delete task by task->tag, regardless of whether it's invoked or not
  virtual void
  deleteTask(TaskPtr task);
  
  // delete task by tag, regardless of whether it's invoked or not
  // if no task is found, no effect
  virtual void
  deleteTask(const Task::Tag &tag);

  // delete tasks by matcher, regardless of whether it's invoked or not
  // this is flexiable in that you can use any form of criteria in finding tasks to delete
  // but keep in mind this is a linear scan

  // if no task is found, no effect
  virtual void
  deleteTask(const Task::TaskMatcher &matcher);

  // task must already have been added to the scheduler, otherwise this method has no effect
  // this is usually used by PeriodicTask
  virtual void
  rescheduleTask(const Task::Tag &tag);

  void
  eventLoop();

  event_base *
  base() { return m_base; }

  // used in test
  int
  size();

protected:
  bool
  addToMap(const TaskPtr &task);

protected:
  typedef std::map<Task::Tag, TaskPtr> TaskMap;
  typedef std::map<Task::Tag, TaskPtr>::iterator TaskMapIt;
  typedef boost::shared_mutex Mutex;
  typedef boost::unique_lock<Mutex> WriteLock;
  typedef boost::shared_lock<Mutex> ReadLock;
  TaskMap m_taskMap;
  Mutex m_mutex;
  bool m_running;
  event_base *m_base;
  boost::thread m_thread;
};

class Scheduler;
typedef boost::shared_ptr<Scheduler> SchedulerPtr;

struct SchedulerException : virtual boost::exception, virtual std::exception { };

#endif // SCHEDULER_H