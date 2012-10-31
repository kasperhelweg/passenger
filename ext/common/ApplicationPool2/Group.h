/*
 *  Phusion Passenger - http://www.modrails.com/
 *  Copyright (c) 2011, 2012 Phusion
 *
 *  "Phusion Passenger" is a trademark of Hongli Lai & Ninh Bui.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *  all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 *  THE SOFTWARE.
 */
#ifndef _PASSENGER_APPLICATION_POOL2_GROUP_H_
#define _PASSENGER_APPLICATION_POOL2_GROUP_H_

#include <string>
#include <map>
#include <queue>
#include <deque>
#include <boost/thread.hpp>
#include <boost/bind.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/make_shared.hpp>
#include <oxt/macros.hpp>
#include <oxt/thread.hpp>
#include <cassert>
#include <ApplicationPool2/Common.h>
#include <ApplicationPool2/ComponentInfo.h>
#include <ApplicationPool2/Spawner.h>
#include <ApplicationPool2/Process.h>
#include <ApplicationPool2/Options.h>
#include <Utils/CachedFileStat.hpp>
#include <Utils/FileChangeChecker.h>
#include <Utils/SmallVector.h>

namespace Passenger {
namespace ApplicationPool2 {

using namespace std;
using namespace boost;
using namespace oxt;


/**
 * Except for otherwise documented parts, this class is not thread-safe,
 * so only access within ApplicationPool lock.
 */
class Group: public enable_shared_from_this<Group> {
private:
	friend class Pool;
	friend class SuperGroup;
	
	struct GetAction {
		GetCallback callback;
		SessionPtr session;
	};
	
	struct DisableWaiter {
		ProcessPtr process;
		DisableCallback callback;
		
		DisableWaiter(const ProcessPtr &_process, const DisableCallback &_callback)
			: process(_process),
			  callback(_callback)
			{ }
	};
	
	mutable boost::mutex backrefSyncher;
	weak_ptr<SuperGroup> superGroup;
	CachedFileStat cstat;
	FileChangeChecker fileChangeChecker;
	string restartFile;
	string alwaysRestartFile;
	
	
	static void _onSessionInitiateFailure(Session *session) {
		const ProcessPtr &process = session->getProcess();
		GroupPtr group = process->getGroup();
		if (OXT_LIKELY(group != NULL)) {
			group->onSessionInitiateFailure(process, session);
		}
	}

	static void _onSessionClose(Session *session) {
		const ProcessPtr &process = session->getProcess();
		GroupPtr group = process->getGroup();
		if (OXT_LIKELY(group != NULL)) {
			group->onSessionClose(process, session);
		}
	}
	
	void createInterruptableThread(const function<void ()> &func, const string &name,
		unsigned int stackSize);
	static string generateSecret(const SuperGroupPtr &superGroup);
	void onSessionInitiateFailure(const ProcessPtr &process, Session *session);
	void onSessionClose(const ProcessPtr &process, Session *session);
	void spawnThreadMain(GroupPtr self, SpawnerPtr spawner, Options options);
	void spawnThreadRealMain(const SpawnerPtr &spawner, const Options &options);
	void finalizeRestart(GroupPtr self, Options options, SpawnerFactoryPtr spawnerFactory,
		vector<Callback> postLockActions);
	
	void verifyInvariants() const {
		// !a || b: logical equivalent of a IMPLIES b.
		
		assert(enabledCount >= 0);
		assert(disablingCount >= 0);
		assert(disabledCount >= 0);
		assert(enabledProcesses.empty() == (pqueue.top() == NULL));
		assert(!( enabledCount == 0 && disablingCount > 0 ) || spawning());
		assert(!( !spawning() ) || ( enabledCount > 0 || disablingCount == 0 ));

		// Verify getWaitlist invariants.
		assert(!( !getWaitlist.empty() ) || ( enabledProcesses.empty() || pqueue.top()->atFullCapacity() ));
		assert(!( !enabledProcesses.empty() && !pqueue.top()->atFullCapacity() ) || ( getWaitlist.empty() ));
		assert(!( enabledProcesses.empty() && !spawning() && !restarting() ) || ( getWaitlist.empty() ));
		assert(!( !getWaitlist.empty() ) || ( !enabledProcesses.empty() || spawning() || restarting() ));
		
		// Verify disableWaitlist invariants.
		assert((int) disableWaitlist.size() >= disablingCount);

		// Verify m_spawning and m_restarting.
		assert(!( m_restarting ) || !m_spawning);
	}

	void verifyExpensiveInvariants() const {
		// !a || b: logical equivalent of a IMPLIES b.

		assert((int) enabledProcesses.size() == enabledCount);
		assert((int) disablingProcesses.size() == disablingCount);
		assert((int) disabledProcesses.size() == disabledCount);
	}
	
	void resetOptions(const Options &newOptions) {
		options = newOptions;
		options.persist(newOptions);
		options.clearPerRequestFields();
		options.groupSecret = secret;
	}
	
	void mergeOptions(const Options &other) {
		options.maxRequests      = other.maxRequests;
		options.minProcesses     = other.minProcesses;
		options.statThrottleRate = other.statThrottleRate;
		options.maxPreloaderIdleTime = other.maxPreloaderIdleTime;
	}
	
	void runAllActions(const vector<Callback> &actions) {
		vector<Callback>::const_iterator it, end = actions.end();
		for (it = actions.begin(); it != end; it++) {
			(*it)();
		}
	}

	static void cleanupSpawner(SpawnerPtr spawner) {
		try {
			spawner->cleanup();
		} catch (const thread_interrupted &) {
			// Return.
		}
	}
	
	SessionPtr newSession(Process *process = NULL) {
		if (process == NULL) {
			assert(enabledCount > 0);
			process = pqueue.top();
		}
		SessionPtr session = process->newSession();
		session->onInitiateFailure = _onSessionInitiateFailure;
		session->onClose   = _onSessionClose;
		if (process->enabled == Process::ENABLED) {
			assert(process == pqueue.top());
			pqueue.pop();
			process->pqHandle = pqueue.push(process, process->utilization());
		}
		return session;
	}

	Process *findProcessWithLowestUtilization(const ProcessList &processes) const {
		Process *result = NULL;
		ProcessList::const_iterator it, end = processes.end();
		for (it = processes.begin(); it != end; it++) {
			Process *process = it->get();
			if (result == NULL || process->utilization() < result->utilization()) {
				result = process;
			}
		}
		return result;
	}

	template<typename Lock>
	void assignSessionsToGetWaitersQuickly(Lock &lock) {
		SmallVector<GetAction, 50> actions;
		actions.reserve(getWaitlist.size());
		
		// Checkout sessions from enabled processes, or if there are none,
		// from disabling processes.
		if (enabledCount > 0) {
			while (!getWaitlist.empty() && pqueue.top() != NULL && !pqueue.top()->atFullCapacity()) {
				GetAction action;
				action.callback = getWaitlist.front().callback;
				action.session  = newSession();
				getWaitlist.pop();
				actions.push_back(action);
			}
		} else if (disablingCount > 0) {
			bool done = false;
			while (!getWaitlist.empty() && !done) {
				Process *process = findProcessWithLowestUtilization(
					disablingProcesses);
				assert(process != NULL);
				if (process->atFullUtilization()) {
					done = true;
				} else {
					GetAction action;
					action.callback = getWaitlist.front().callback;
					action.session  = newSession(process);
					getWaitlist.pop();
					actions.push_back(action);
				}
			}
		}
		
		verifyInvariants();
		lock.unlock();
		SmallVector<GetAction, 50>::const_iterator it, end = actions.end();
		for (it = actions.begin(); it != end; it++) {
			it->callback(it->session, ExceptionPtr());
		}
	}
	
	void assignSessionsToGetWaiters(vector<Callback> &postLockActions) {
		if (enabledCount > 0) {
			while (!getWaitlist.empty() && pqueue.top() != NULL && !pqueue.top()->atFullCapacity()) {
				postLockActions.push_back(boost::bind(
					getWaitlist.front().callback, newSession(),
					ExceptionPtr()));
				getWaitlist.pop();
			}
		} else if (disablingCount > 0) {
			bool done = false;
			while (!getWaitlist.empty() && !done) {
				Process *process = findProcessWithLowestUtilization(
					disablingProcesses);
				assert(process != NULL);
				if (process->atFullUtilization()) {
					done = true;
				} else {
					postLockActions.push_back(boost::bind(
						getWaitlist.front().callback, newSession(process),
						ExceptionPtr()));
					getWaitlist.pop();
				}
			}
		}
	}
	
	void assignExceptionToGetWaiters(const ExceptionPtr &exception,
		vector<Callback> &postLockActions)
	{
		while (!getWaitlist.empty()) {
			postLockActions.push_back(boost::bind(
				getWaitlist.front().callback, SessionPtr(),
				exception));
			getWaitlist.pop();
		}
	}

	void enableAllDisablingProcesses(vector<Callback> &postLockActions) {
		deque<DisableWaiter>::iterator it, end = disableWaitlist.end();
		for (it = disableWaitlist.begin(); it != end; it++) {
			const DisableWaiter &waiter = *it;
			const ProcessPtr process = waiter.process;
			assert(process->enabled == Process::DISABLING);
			process->enabled = Process::ENABLED;
			disablingProcesses.erase(process->it);
			enabledProcesses.push_back(process);
			process->it = enabledProcesses.last_iterator();
			process->pqHandle = pqueue.push(process.get(), process->utilization());
			disablingCount--;
			enabledCount++;
		}
		clearDisableWaitlist(DR_ERROR, postLockActions);
	}
	
	void removeFromDisableWaitlist(const ProcessPtr &p, DisableResult result,
		vector<Callback> &postLockActions)
	{
		deque<DisableWaiter>::const_iterator it, end = disableWaitlist.end();
		deque<DisableWaiter> newList;
		for (it = disableWaitlist.begin(); it != end; it++) {
			const DisableWaiter &waiter = *it;
			const ProcessPtr process = waiter.process;
			if (process == p) {
				postLockActions.push_back(boost::bind(waiter.callback, p, result));
			} else {
				newList.push_back(waiter);
			}
		}
		disableWaitlist = newList;
	}

	void clearDisableWaitlist(DisableResult result, vector<Callback> &postLockActions) {
		postLockActions.reserve(postLockActions.size() + disableWaitlist.size());
		while (!disableWaitlist.empty()) {
			const DisableWaiter &waiter = disableWaitlist.front();
			assert(waiter.process->enabled == Process::DISABLING);
			postLockActions.push_back(boost::bind(waiter.callback, waiter.process, result));
			disableWaitlist.pop_front();
		}
	}
	
public:
	Options options;
	/** This name uniquely identifies this Group within its Pool. It can also be used as the display name. */
	const string name;
	/** A secret token that may be known among all processes in this Group. Used for securing
	 * intra-group process communication.
	 */
	const string secret;
	ComponentInfo componentInfo;
	
	/**
	 * Processes are categorized as enabled, disabling or disabled.
	 *
	 * - get() requests should go to enabled processes.
	 * - Disabling processes are allowed to finish their current requests,
	 *   but they generally will not receive any new requests. The only
	 *   exception is when there are no enabled processes. In this case,
	 *   a new process will be spawned while in the mean time all requests
	 *   go to one of the disabling processes. Disabling processes become
	 *   disabled as soon as they finish all their requests and there are
	 *   enabled processes.
	 * - Disabled processes never handle requests.
	 *
	 * 'enabledProcesses', 'disablingProcesses' and 'disabledProcesses' contain
	 * all enabled, disabling and disabling processes in this group, respectively.
	 * 'enabledCount', 'disablingCount' and 'disabledCount' are used to maintain
	 * their numbers.
	 * These lists do not intersect. A process is in exactly 1 list.
	 *
	 * 'pqueue' orders all enabled processes according to utilization() values,
	 * from small to large.
	 *
	 * Invariants:
	 *    enabledCount >= 0
	 *    disablingCount >= 0
	 *    disabledCount >= 0
	 *    enabledProcesses.size() == enabledCount
	 *    disablingProcesses.size() == disabingCount
	 *    disabledProcesses.size() == disabledCount
     *
	 *    enabledProcesses.empty() == (pqueue.top() == NULL)
	 *
	 *    if (enabledCount == 0) and (disablingCount > 0):
	 *       spawning()
	 *    if !spawning():
	 *       (enabledCount > 0) or (disablingCount == 0)
	 *
	 *    if pqueue.top().atFullCapacity():
	 *       All enabled processes are at full capacity.
	 *
	 *    for all process in enabledProcesses:
	 *       process.enabled == Process::ENABLED
	 *    for all processes in disablingProcesses:
	 *       process.enabled == Process::DISABLING
	 *    for all process in disabledProcesses:
	 *       process.enabled == Process::DISABLED
	 */
	int enabledCount;
	int disablingCount;
	int disabledCount;
	PriorityQueue<Process> pqueue;
	ProcessList enabledProcesses;
	ProcessList disablingProcesses;
	ProcessList disabledProcesses;
	
	/**
	 * get() requests for this group that cannot be immediately satisfied are
	 * put on this wait list, which must be processed as soon as the necessary
	 * resources have become free.
	 *
	 * Invariant 1:
	 *    if getWaitlist is non-empty:
	 *       enabledProcesses.empty() or (all enabled processes are at full capacity)
	 * Equivalently:
	 *    if !enabledProcesses.empty() and (an enabled process is not at full capacity):
	 *        getWaitlist is empty.
	 *
	 * Invariant 2:
	 *    if enabledProcesses.empty() && !spawning() && !restarting():
	 *       getWaitlist is empty
	 * Equivalently:
	 *    if getWaitlist is non-empty:
	 *       !enabledProcesses.empty() || spawning() || restarting()
	 */
	queue<GetWaiter> getWaitlist;
	/**
	 * Invariant:
	 *    disableWaitlist.size() >= disablingCount
	 */
	deque<DisableWaiter> disableWaitlist;
	
	SpawnerPtr spawner;
	/**
	 * Whether process(es) are being spawned right now.
	 */
	bool m_spawning;
	/** Whether a non-rolling restart is in progress. While it is in progress,
	 * it is not possible to signal the desire to spawn new process. If spawning
	 * was already in progress when the restart was initiated, then the spawning
	 * will abort as soon as possible.
	 *
	 * When rolling restarting is in progress, this flag is false.
	 *
	 * Invariant:
	 *    if m_restarting: !m_spawning
	 */
	bool m_restarting;
	
	Group(const SuperGroupPtr &superGroup, const Options &options, const ComponentInfo &info);
	
	SessionPtr get(const Options &newOptions, const GetCallback &callback) {
		if (OXT_LIKELY(!restarting())) {
			if (OXT_UNLIKELY(needsRestart(newOptions))) {
				restart(newOptions);
			} else {
				mergeOptions(newOptions);
			}
			if (OXT_UNLIKELY(!newOptions.noop && shouldSpawn())) {
				spawn();
			}
		}
		
		if (OXT_UNLIKELY(newOptions.noop)) {
			ProcessPtr process = make_shared<Process>(SafeLibevPtr(),
				0, string(), string(),
				FileDescriptor(), FileDescriptor(),
				SocketListPtr(), 0, 0);
			process->setGroup(shared_from_this());
			return make_shared<Session>(process, (Socket *) NULL);
		}
		
		if (OXT_UNLIKELY(enabledCount == 0)) {
			/* We don't have any processes yet, but they're on the way.
			 *
			 * We have some choices here. If there are disabling processes
			 * then we generally want to use them, except:
			 * - When non-rolling restarting because those disabling processes
			 *   are from the old version.
			 * - When all disabling processes are at full utilization.
			 *
			 * Whenever a disabling process cannot be used, call the callback
			 * after a process has been spawned or has failed to spawn, or
			 * when a disabling process becomes available.
			 */
			assert(spawning() || restarting());

			if (disablingCount > 0 && !restarting()) {
				Process *process = findProcessWithLowestUtilization(
					disablingProcesses);
				assert(process != NULL);
				if (!process->atFullUtilization()) {
					return newSession(process);
				}
			}

			getWaitlist.push(GetWaiter(newOptions, callback));
			P_DEBUG("No session checked out yet: group is spawning or restarting");
			return SessionPtr();
		} else {
			Process *process = pqueue.top();
			assert(process != NULL);
			if (process->atFullCapacity()) {
				/* Looks like all processes are at full capacity.
				 * Wait until a new one has been spawned or until
				 * resources have become free.
				 */
				getWaitlist.push(GetWaiter(newOptions, callback));
				P_DEBUG("No session checked out yet: all processes are at full capacity");
				return SessionPtr();
			} else {
				P_DEBUG("Session checked out from process " << process->inspect());
				return newSession();
			}
		}
	}
	
	// Thread-safe.
	SuperGroupPtr getSuperGroup() const {
		lock_guard<boost::mutex> lock(backrefSyncher);
		return superGroup.lock();
	}
	
	// Thread-safe.
	void setSuperGroup(const SuperGroupPtr &superGroup) {
		lock_guard<boost::mutex> lock(backrefSyncher);
		this->superGroup = superGroup;
	}
	
	// Thread-safe.
	PoolPtr getPool() const;
	
	// Thread-safe.
	bool detached() const {
		return getSuperGroup() == NULL;
	}
	
	/**
	 * Attaches the given process to this Group and mark it as enabled. This
	 * function doesn't touch getWaitlist so be sure to fix its invariants
	 * afterwards if necessary.
	 */
	void attach(const ProcessPtr &process, vector<Callback> &postLockActions) {
		assert(process->getGroup() == NULL);
		process->setGroup(shared_from_this());
		enabledProcesses.push_back(process);
		process->it = enabledProcesses.last_iterator();
		process->pqHandle = pqueue.push(process.get(), process->utilization());
		process->enabled = Process::ENABLED;
		enabledCount++;
		
		/* Now that there are enough resources all process in 'disableWaitlist'
		 * can be disabled.
		 */
		deque<DisableWaiter>::const_iterator it, end = disableWaitlist.end();
		postLockActions.reserve(postLockActions.size() + disableWaitlist.size());
		for (it = disableWaitlist.begin(); it != end; it++) {
			const DisableWaiter &waiter = *it;
			const ProcessPtr process2 = waiter.process;
			// The same process can appear multiple times in disableWaitlist.
			assert(process2->enabled == Process::DISABLING
				|| process2->enabled == Process::DISABLED);
			if (process2->enabled == Process::DISABLING) {
				process2->enabled = Process::DISABLED;
				disablingProcesses.erase(process2->it);
				disabledProcesses.push_back(process2);
				process2->it = disabledProcesses.last_iterator();
				disablingCount--;
				disabledCount++;
			}
			postLockActions.push_back(boost::bind(waiter.callback, process2, DR_SUCCESS));
		}
		disableWaitlist.clear();
	}

	/**
	 * Detaches the given process from this Group. This function doesn't touch
	 * getWaitlist so be sure to fix its invariants afterwards if necessary.
	 */
	void detach(const ProcessPtr &process, vector<Callback> &postLockActions) {
		assert(process->getGroup().get() == this);

		process->setGroup(GroupPtr());

		if (process->enabled == Process::ENABLED || process->enabled == Process::DISABLING) {
			assert(enabledCount > 0 || disablingCount > 0);
			if (process->enabled == Process::ENABLED) {
				pqueue.erase(process->pqHandle);
				enabledProcesses.erase(process->it);
				enabledCount--;
			} else {
				disablingProcesses.erase(process->it);
				disablingCount--;
				removeFromDisableWaitlist(process, DR_NOOP, postLockActions);
			}
		} else {
			assert(!disabledProcesses.empty());
			disabledProcesses.erase(process->it);
			disabledCount--;
		}
	}
	
	/**
	 * Detaches all processes from this Group. This function doesn't touch
	 * getWaitlist so be sure to fix its invariants afterwards if necessary.
	 */
	void detachAll(vector<Callback> &postLockActions) {
		ProcessList::iterator it, end = enabledProcesses.end();
		for (it = enabledProcesses.begin(); it != end; it++) {
			(*it)->setGroup(GroupPtr());
		}
		end = disablingProcesses.end();
		for (it = disablingProcesses.begin(); it != end; it++) {
			(*it)->setGroup(GroupPtr());
		}
		end = disabledProcesses.end();
		for (it = disabledProcesses.begin(); it != end; it++) {
			(*it)->setGroup(GroupPtr());
		}
		
		enabledProcesses.clear();
		disablingProcesses.clear();
		disabledProcesses.clear();
		pqueue.clear();
		enabledCount = 0;
		disablingCount = 0;
		disabledCount = 0;
		clearDisableWaitlist(DR_NOOP, postLockActions);
	}
	
	/**
	 * Marks the given process as enabled. This function doesn't touch getWaitlist
	 * so be sure to fix its invariants afterwards if necessary.
	 */
	void enable(const ProcessPtr &process, vector<Callback> &postLockActions) {
		assert(process->getGroup().get() == this);
		if (process->enabled == Process::DISABLING) {
			process->enabled = Process::ENABLED;
			disablingProcesses.erase(process->it);
			enabledProcesses.push_back(process);
			process->it = enabledProcesses.last_iterator();
			process->pqHandle = pqueue.push(process.get(), process->utilization());
			disablingCount--;
			enabledCount++;
			removeFromDisableWaitlist(process, DR_CANCELED, postLockActions);
		} else if (process->enabled == Process::DISABLED) {
			process->enabled = Process::ENABLED;
			disabledProcesses.erase(process->it);
			enabledProcesses.push_back(process);
			process->it = enabledProcesses.last_iterator();
			process->pqHandle = pqueue.push(process.get(), process->utilization());
			disabledCount--;
			enabledCount++;
		}
	}
	
	/**
	 * Marks the given process as disabled. Returns DR_SUCCESS, DR_DEFERRED
	 * or DR_NOOP. If the result is DR_DEFERRED, then the callback will be
	 * called later with the result of this action.
	 */
	DisableResult disable(const ProcessPtr &process, const DisableCallback &callback) {
		assert(process->getGroup().get() == this);
		if (process->enabled == Process::ENABLED) {
			assert(enabledCount > 0);
			if (enabledCount == 1) {
				/* All processes are going to be disabled, so in order
				 * to avoid blocking requests we first spawn a new process
				 * and disable this process after the other one is done
				 * spawning. We do this irregardless of resource limits
				 * because this is an exceptional situation.
				 */
				process->enabled = Process::DISABLING;
				enabledProcesses.erase(process->it);
				disablingProcesses.push_back(process);
				process->it = disablingProcesses.last_iterator();
				disablingCount++;
				enabledCount--;
				disableWaitlist.push_back(DisableWaiter(process, callback));
				spawn();
				return DR_DEFERRED;
			} else {
				assert(enabledCount > 1);
				enabledProcesses.erase(process->it);
				disabledProcesses.push_back(process);
				pqueue.erase(process->pqHandle);
				process->it = disabledProcesses.last_iterator();
				process->enabled = Process::DISABLED;
				enabledCount--;
				disabledCount++;
				return DR_SUCCESS;
			}
		} else if (process->enabled == Process::DISABLING) {
			assert(disablingCount > 0);
			disableWaitlist.push_back(DisableWaiter(process, callback));
			return DR_DEFERRED;
		} else {
			assert(disabledCount > 0);
			return DR_NOOP;
		}
	}

	void asyncCleanupSpawner() {
		createInterruptableThread(
			boost::bind(cleanupSpawner, spawner),
			"Group spawner cleanup: " + name,
			POOL_HELPER_THREAD_STACK_SIZE);
	}

	unsigned int utilization() const {
		int result = enabledCount;
		if (spawning()) {
			result++;
		}
		return result;
	}
	
	bool garbageCollectable(unsigned long long now = 0) const {
		/* if (now == 0) {
			now = SystemTime::getUsec();
		}
		return utilization() == 0
			&& getWaitlist.empty()
			&& disabledProcesses.empty()
			&& options.getMaxPreloaderIdleTime() != 0
			&& now - spawner->lastUsed() >
				(unsigned long long) options.getMaxPreloaderIdleTime() * 1000000; */
		return false;
	}
	
	/** Whether a new process should be spawned for this group in case
	 * another get action is to be performed.
	 */
	bool shouldSpawn() const;
	
	/** Start spawning a new process in the background, in case this
	 * isn't already happening and the group isn't being restarted.
	 * Will ensure that at least options.minProcesses processes are spawned.
	 */
	void spawn() {
		if (!spawning() && !restarting()) {
			P_DEBUG("Requested spawning of new process for group " << name);
			createInterruptableThread(
				boost::bind(&Group::spawnThreadMain,
					this, shared_from_this(), spawner,
					options.copyAndPersist().clearPerRequestFields()),
				"Group process spawner: " + name,
				POOL_HELPER_THREAD_STACK_SIZE);
			m_spawning = true;
		}
	}
	
	bool needsRestart(const Options &options) {
		if (m_restarting) {
			return false;
		} else {
			struct stat buf;
			return cstat.stat(alwaysRestartFile, &buf, options.statThrottleRate) == 0 ||
			       fileChangeChecker.changed(restartFile, options.statThrottleRate);
		}
	}
	
	void restart(const Options &options);
	
	bool spawning() const {
		return m_spawning;
	}

	bool restarting() const {
		return m_restarting;
	}

	template<typename Stream>
	void inspectXml(Stream &stream, bool includeSecrets = true) const {
		ProcessList::const_iterator it;

		stream << "<name>" << escapeForXml(name) << "</name>";
		stream << "<component_name>" << escapeForXml(componentInfo.name) << "</component_name>";
		stream << "<app_root>" << escapeForXml(options.appRoot) << "</app_root>";
		stream << "<app_type>" << escapeForXml(options.appType) << "</app_type>";
		stream << "<environment>" << escapeForXml(options.environment) << "</environment>";
		stream << "<enabled_process_count>" << enabledCount << "</enabled_process_count>";
		stream << "<disabling_process_count>" << disablingCount << "</disabling_process_count>";
		stream << "<disabled_process_count>" << disabledCount << "</disabled_process_count>";
		stream << "<utilization>" << utilization() << "</utilization>";
		stream << "<get_wait_list_size>" << getWaitlist.size() << "</get_wait_list_size>";
		stream << "<disable_wait_list_size>" << disableWaitlist.size() << "</disable_wait_list_size>";
		if (spawning()) {
			stream << "<spawning/>";
		}
		if (restarting()) {
			stream << "<restarting/>";
		}
		if (includeSecrets) {
			stream << "<secret>" << escapeForXml(secret) << "</secret>";
		}

		stream << "<processes>";
		
		for (it = enabledProcesses.begin(); it != enabledProcesses.end(); it++) {
			stream << "<process>";
			(*it)->inspectXml(stream, includeSecrets);
			stream << "</process>";
		}
		for (it = disablingProcesses.begin(); it != disablingProcesses.end(); it++) {
			stream << "<process>";
			(*it)->inspectXml(stream, includeSecrets);
			stream << "</process>";
		}
		for (it = disabledProcesses.begin(); it != disabledProcesses.end(); it++) {
			stream << "<process>";
			(*it)->inspectXml(stream, includeSecrets);
			stream << "</process>";
		}

		stream << "</processes>";
	}
};


} // namespace ApplicationPool2
} // namespace Passenger

#endif /* _PASSENGER_APPLICATION_POOL2_GROUP_H_ */