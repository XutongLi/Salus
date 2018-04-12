/*
 * <one line to give the library's name and an idea of what it does.>
 * Copyright (C) 2017  Aetf <aetf@unlimitedcodeworks.xyz>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "executionengine.h"

#include "execution/operationtask.h"
#include "execution/scheduler/basescheduler.h"
#include "execution/scheduler/operationitem.h"
#include "execution/scheduler/sessionitem.h"
#include "platform/logging.h"
#include "utils/containerutils.h"
#include "utils/date.h"
#include "utils/debugging.h"
#include "utils/envutils.h"
#include "utils/macros.h"
#include "utils/pointerutils.h"
#include "utils/threadutils.h"

#include <algorithm>
#include <functional>
#include <iomanip>

using std::chrono::duration_cast;
using std::chrono::microseconds;
using std::chrono::milliseconds;
using std::chrono::nanoseconds;
using std::chrono::seconds;
using std::chrono::system_clock;
using FpSeconds = std::chrono::duration<double, seconds::period>;
using namespace std::chrono_literals;
using namespace date;
using namespace salus;

namespace {
inline void logScheduleFailure(const Resources &usage, const ResourceMonitor &resMon)
{
    UNUSED(usage);
    UNUSED(resMon);

#ifndef NDEBUG
    VLOG(2) << "Try to allocate resource failed. Requested: " << usage;
    // Don't call resMon.DebugString directly in log line, as logging acquires lock, and
    // may causing deadlock.
    const auto &str = resMon.DebugString();
    VLOG(2) << "Available: " << str;
#endif
}

} // namespace

ExecutionEngine &ExecutionEngine::instance()
{
    static ExecutionEngine eng;
    return eng;
}

void ExecutionEngine::startScheduler()
{
    // Start scheduling thread
    m_schedThread = std::make_unique<std::thread>(std::bind(&ExecutionEngine::scheduleLoop, this));
}

void ExecutionEngine::stopScheduler()
{
    // stop scheduling thread
    m_shouldExit = true;
    // also unblock scheduling thread
    m_note_has_work.notify();
    m_schedThread->join();

    // remove any pending new or delete session
    // NOTE: has to be done *after* the scheduling thread exits.
    m_newSessions.clear();
    m_deletedSessions.clear();
}

ExecutionEngine::~ExecutionEngine()
{
    stopScheduler();
}

ExecutionContext ExecutionEngine::createSessionOffer(ResourceMap rm)
{
    uint64_t offer;
    if (!SessionResourceTracker::instance().admit(rm, offer)) {
        LOG(WARNING) << "Rejecting session due to unsafe resource usage. Predicted usage: "
                     << rm.DebugString()
                     << ", current usage: " << SessionResourceTracker::instance().DebugString();
        return {};
    }

    // session handle is set later in acceptOffer.
    return {std::make_shared<SessionItem>(""), offer, *this};
}

void ExecutionContext::acceptOffer(const std::string &sessHandle)
{
    DCHECK(m_data);
    SessionResourceTracker::instance().acceptAdmission(m_data->resOffer, sessHandle);
    m_data->item->sessHandle = sessHandle;
    m_data->insertIntoEngine();
}

std::optional<ResourceMap> ExecutionContext::offeredSessionResource() const
{
    DCHECK(m_data);
    return SessionResourceTracker::instance().usage(m_data->resOffer);
}

void ExecutionEngine::insertSession(PSessionItem item)
{
    {
        std::lock_guard<std::mutex> g(m_newMu);
        m_newSessions.emplace_back(std::move(item));
    }
    m_note_has_work.notify();
}

void ExecutionEngine::deleteSession(PSessionItem item)
{
    {
        std::lock_guard<std::mutex> g(m_delMu);
        m_deletedSessions.emplace(std::move(item));
    }
    m_note_has_work.notify();
}

void ExecutionContext::enqueueOperation(std::unique_ptr<OperationTask> &&task)
{
    DCHECK(m_data);
    DCHECK(m_data->item);
    m_data->enqueueOperation(std::forward<std::unique_ptr<OperationTask>>(task));
}

void ExecutionContext::Data::enqueueOperation(std::unique_ptr<OperationTask> &&task)
{
    auto opItem = std::make_shared<OperationItem>();
    opItem->sess = item;
    opItem->op = std::move(task);
    LogOpTracing() << "OpItem Event " << opItem->op << " event: queued";

    engine.pushToSessionQueue(std::move(opItem));
}

void ExecutionContext::registerPagingCallbacks(PagingCallbacks &&pcb)
{
    DCHECK(m_data);
    DCHECK(m_data->item);
    m_data->item->setPagingCallbacks(std::move(pcb));
}

void ExecutionContext::deleteSession(std::function<void()> cb)
{
    DCHECK(m_data);
    DCHECK(m_data->item);

    m_data->item->prepareDelete(std::move(cb));

    // Request engine to remove session and give up our reference to the session item
    m_data->removeFromEngine();
}

std::unique_ptr<ResourceContext> ExecutionContext::makeResourceContext(const salus::DeviceSpec &spec,
                                                                       const Resources &res,
                                                                       Resources *missing)
{
    DCHECK(m_data);

    return m_data->makeResourceContext(spec, res, missing);
}

std::unique_ptr<ResourceContext> ExecutionContext::Data::makeResourceContext(const salus::DeviceSpec &spec,
                                                                             const Resources &res,
                                                                             Resources *missing)
{
    DCHECK(item);
    return engine.makeResourceContext(item, spec, res, missing);
}

void ExecutionEngine::pushToSessionQueue(POpItem &&opItem)
{
    auto sess = opItem->sess.lock();
    if (!sess) {
        // session already deleted, discard this task sliently
        return;
    }

    {
        auto g = sstl::with_guard(sess->mu);
        sess->queue.emplace_back(std::move(opItem));
    }
    m_note_has_work.notify();
}
void ExecutionContext::Data::insertIntoEngine()
{
    if (item) {
        engine.insertSession(item);
    }
}

void ExecutionContext::Data::removeFromEngine()
{
    if (item) {
        engine.deleteSession(std::move(item));
    }
}

ExecutionContext::Data::~Data()
{
    removeFromEngine();

    if (resOffer) {
        SessionResourceTracker::instance().free(resOffer);
    }
}

bool ExecutionEngine::maybeWaitForAWhile(size_t scheduled)
{
    constexpr auto initialSleep = 10ms;
    constexpr auto getBored = 20ms;

    static auto last = system_clock::now();
    static auto sleep = initialSleep;

    auto now = system_clock::now();

    if (scheduled > 0) {
        last = now;
        sleep = initialSleep;
    }

    auto idle = now - last;
    if (idle <= getBored) {
        return false;
    }

    VLOG(2) << "No progress for " << duration_cast<milliseconds>(idle).count() << "ms, sleep for "
            << duration_cast<milliseconds>(sleep).count() << "ms";

    // no progress for a long time.
    // give out our time slice to avoid using too much cycles
    //             std::this_thread::yield();
    std::this_thread::sleep_for(sleep);

    // Next time we'll sleep longer
    sleep *= 2;

    return true;
}

void ExecutionEngine::scheduleLoop()
{
    m_resMonitor.initializeLimits();
    auto scheduler = SchedulerRegistary::instance().create(m_schedParam.scheduler, *this);
    DCHECK(scheduler);
    VLOG(2) << "Using scheduler: " << scheduler;

    m_runningTasks = 0;
    m_noPagingRunningTasks = 0;

    size_t schedIterCount = 0;
    const auto kNameBufLen = 256;
    char schedIterNameBuf[kNameBufLen];
    boost::container::small_vector<PSessionItem, 5> candidates;

    while (!m_shouldExit) {
        snprintf(schedIterNameBuf, kNameBufLen, "sched-iter-%zu", schedIterCount++);
        SessionChangeSet changeset;
        // Fisrt check if there's any pending deletions
        {
            auto g = sstl::with_guard(m_delMu);

            using std::swap;
            swap(changeset.deletedSessions, m_deletedSessions);
            DCHECK(m_deletedSessions.empty());
        }

        // Delete sessions as requested
        // NOTE: don't clear del yet, we need that in changeset for scheduling
        m_sessions.remove_if([&changeset](auto sess) {
            bool deleted = changeset.deletedSessions.count(sess) > 0;
            if (deleted) {
                VLOG(2) << "Deleting session " << sess->sessHandle << "@" << as_hex(sess);
                // The deletion of session's executor is async to this thread.
                // So it's legit for tickets to be nonempty
                // DCHECK(item->tickets.empty());
            }
            return deleted;
        });

        // Append any new sessions
        {
            auto g = sstl::with_guard(m_newMu);

            changeset.numAddedSessions = m_newSessions.size();

            // list::splice doesn't invalidate iterators, so use
            // m_newSessions.begin() here is ok, and a must.
            changeset.addedSessionBegin = m_newSessions.begin();
            changeset.addedSessionEnd = m_sessions.end();

            m_sessions.splice(m_sessions.end(), m_newSessions);
            DCHECK(m_newSessions.empty());
        }

        // Prepare session ready for this iter of schedule:
        // - move from front end queue to backing storage
        // - reset lastScheduled
        size_t totalRemainingCount = 0;
        bool enableOOMProtect = m_sessions.size() > 1;
        for (auto &item : m_sessions) {
            {
                auto g = sstl::with_guard(item->mu);
                item->bgQueue.splice(item->bgQueue.end(), item->queue);
            }

            if (item->forceEvicted) {
                VLOG(2) << "Canceling pending tasks in forced evicted seesion: " << item->sessHandle;
                // cancel all pending tasks
                for (auto &opItem : item->bgQueue) {
                    opItem->op->cancel();
                }
                item->bgQueue.clear();
            }

            totalRemainingCount += item->bgQueue.size();

            item->protectOOM = enableOOMProtect;
            item->lastScheduled = 0;
        }

        // Select and sort candidates.
        scheduler->notifyPreSchedulingIteration(m_sessions, changeset, &candidates);

        // Deleted sessions are no longer needed, release them.
        changeset.deletedSessions.clear();

        // Schedule tasks from candidate sessions
        // NOTE: remainingCount only counts for candidate sessions in this sched iter.
        size_t remainingCount = 0;
        size_t scheduled = 0;
        for (auto &item : candidates) {
            VLOG(3) << "Scheduling all opItem in session " << item->sessHandle << ": queue size "
                    << item->bgQueue.size();

            // Try schedule from this session
            auto [count, shouldContinue] = scheduler->maybeScheduleFrom(item);
            item->lastScheduled = count;

            remainingCount += item->bgQueue.size();
            scheduled += item->lastScheduled;

            if (!shouldContinue) {
                break;
            }
        }

        // Log performance counters
        CLOG(INFO, logging::kPerfTag)
            << "Scheduler iter stat: " << schedIterCount << " running: " << m_runningTasks
            << " noPageRunning: " << m_noPagingRunningTasks;
        for (auto &item : m_sessions) {
            CLOG(INFO, logging::kPerfTag)
                << "Sched iter " << schedIterCount << " session: " << item->sessHandle
                << " pending: " << item->bgQueue.size() << " scheduled: " << item->lastScheduled << " "
                << scheduler->debugString(item);
        }

        // Update conditions and check if we need paging
        bool noProgress = remainingCount > 0 && scheduled == 0 && m_noPagingRunningTasks == 0;
        bool didPaging = false;
        // TODO: we currently assume we are paging GPU memory to CPU
        for (const auto &dev : {devices::GPU0}) {
            if (noProgress && scheduler->insufficientMemory(dev)) {
                if (m_sessions.size() > 1) {
                    didPaging = doPaging(dev, devices::CPU0);
                } else if (m_sessions.size() == 1) {
                    LOG(ERROR) << "OOM on device " << dev
                               << " for single session happened: " << m_sessions.front()->sessHandle;
                    {
                        auto g = sstl::with_guard(m_sessions.front()->tickets_mu);
                        auto usage = m_resMonitor.queryUsages(m_sessions.front()->tickets);
                        LOG(ERROR) << "This session usage:" << resources::DebugString(usage);
                    }
                    LOG(ERROR) << m_resMonitor.DebugString();
                }
            }
        }
        // succeed, retry another sched iter immediately
        if (didPaging) {
            continue;
        }

        maybeWaitForAWhile(scheduled);

        if (!totalRemainingCount) {
            VLOG(2) << "Wait on m_note_has_work";
            m_note_has_work.wait();
        }
    }

    // Cleanup
    m_sessions.clear();
}

std::unique_ptr<ResourceContext> ExecutionEngine::makeResourceContext(PSessionItem sess,
                                                                      const DeviceSpec &spec,
                                                                      const Resources &res,
                                                                      Resources *missing)
{
    auto rctx = std::make_unique<ResourceContext>(std::move(sess), m_resMonitor);
    if (!rctx->initializeStaging(spec, res, missing)) {
        logScheduleFailure(res, m_resMonitor);
    }
    return rctx;
}

POpItem ExecutionEngine::submitTask(POpItem &&opItem)
{
    auto item = opItem->sess.lock();
    if (!item) {
        // discard
        return nullptr;
    }

    if (!opItem->op->resourceContext().isGood()) {
        LOG(ERROR) << "Submitted task with uninitialized resource context: " << opItem->op->DebugString()
                   << " in session " << item->sessHandle;
        return std::move(opItem);
    }

    // NOTE: this is waited by schedule thread, so we can't afford running
    // the operation inline. If the thread pool is full, simply consider the
    // opItem as not scheduled.

    // opItem has to be captured by value, we need it in case the thread pool is full
    auto c = m_pool.tryRun([opItem, this]() mutable {
        DCHECK(opItem);

        if (auto item = opItem->sess.lock()) {
            OperationTask::Callbacks cbs;

            // capture an session item untile done
            cbs.done = [item, opItem, this]() {
                // succeed
                taskStopped(*opItem, false);
            };
            cbs.memFailure = [opItem, this]() mutable {
                auto item = opItem->sess.lock();
                if (!item) {
                    VLOG(2) << "Found expired session during handling of memory failure of opItem: "
                            << opItem->op;
                    return false;
                }
                if (!item->protectOOM) {
                    VLOG(2) << "Pass through OOM failed task back to client: " << opItem->op;
                    return false;
                }

                taskStopped(*opItem, true);
                // failed due to OOM. Push back to queue and retry later
                VLOG(2) << "Putting back OOM failed task: " << opItem->op;
                pushToSessionQueue(std::move(opItem));
                return true;
            };

            VLOG(2) << "Running opItem in session " << item->sessHandle << ": " << opItem->op;
            taskRunning(*opItem);
            opItem->op->run(std::move(cbs));
        }
    });
    if (!c) {
        // successfully sent to thread pool, we can reset opItem
        opItem.reset();
    }
    return opItem;
}

void ExecutionEngine::taskRunning(OperationItem &opItem)
{
    LogOpTracing() << "OpItem Event " << opItem.op << " event: running";
    m_runningTasks += 1;
    if (!opItem.op->isAsync()) {
        m_noPagingRunningTasks += 1;
    }
}

void ExecutionEngine::taskStopped(OperationItem &opItem, bool failed)
{
    auto &rctx = opItem.op->resourceContext();
    rctx.releaseStaging();

    LogOpTracing() << "OpItem Event " << opItem.op << " event: done";
    if (!failed) {
        if (VLOG_IS_ON(2)) {
            if (auto item = opItem.sess.lock(); item) {
                auto g = sstl::with_guard(item->mu);
                ++item->totalExecutedOp;
            }
        }
    }

    m_runningTasks -= 1;
    if (!opItem.op->isAsync()) {
        m_noPagingRunningTasks -= 1;
    }
}

bool ExecutionEngine::doPaging(const DeviceSpec &spec, const DeviceSpec &target)
{
    auto now = system_clock::now();
    size_t released = 0;
    std::string forceEvicitedSess;

    sstl::ScopeGuards sg([&now, &released, &forceEvicitedSess]() {
        auto dur = system_clock::now() - now;
        CLOG(INFO, logging::kPerfTag)
            << "Paging: "
            << " duration: " << duration_cast<microseconds>(dur).count() << " us"
            << " released: " << released << " forceevict: '" << forceEvicitedSess << "'";
    });

    const ResourceTag srcTag{ResourceType::MEMORY, spec};
    const ResourceTag dstTag{ResourceType::MEMORY, target};

    // Step 1: select candidate sessions
    std::vector<std::pair<size_t, std::reference_wrapper<PSessionItem>>> candidates;
    candidates.reserve(m_sessions.size());

    // Step 1.1: count total memory usage for each session
    for (auto &pSess : m_sessions) {
        candidates.emplace_back(pSess->resourceUsage(srcTag), pSess);
    }

    // sort in decending order
    std::sort(candidates.begin(), candidates.end(),
              [](const auto &lhs, const auto &rhs) { return lhs.first > rhs.first; });

    // Step 1.2: keep the session with largest memory usage, and try from next
    // no need to erase the first elem, as it's a O(n) operation on vector

    if (candidates.size() <= 1) {
        LOG(ERROR) << "Out of memory for one session";
        return false;
    }

    if (VLOG_IS_ON(2)) {
        for (auto [usage, pSess] : candidates) {
            VLOG(2) << "Session " << pSess.get()->sessHandle << " usage: " << usage;
        }
    }

    // Step 2: inform owner to do paging given suggestion
    for (size_t i = 1; i != candidates.size(); ++i) {
        auto &pSess = candidates[i].second.get();
        std::vector<std::pair<size_t, uint64_t>> victims;
        {
            auto g = sstl::with_guard(pSess->tickets_mu);
            if (pSess->tickets.empty()) {
                // no need to go beyond
                break;
            }
            victims = m_resMonitor.sortVictim(pSess->tickets);
        }

        // we will be doing paging on this session. Lock it's input queue lock
        // also prevents the executor from clearing the paging callbacks.
        // This should not create deadlock as nothing could finish at this time,
        // thus no new tasks could be submitted.
        auto g = sstl::with_guard(pSess->mu);
        if (!pSess->pagingCb) {
            continue;
        }

        VLOG(2) << "Visiting session: " << pSess->sessHandle;

        for (auto [usage, victim] : victims) {
            // preallocate some CPU memory for use.
            Resources res{{dstTag, usage}};

            auto rctx = makeResourceContext(pSess, target, res);
            if (!rctx->isGood()) {
                LOG(ERROR) << "No enough CPU memory for paging. Required: " << res[dstTag] << " bytes";
                return false;
            }
            LogAlloc() << "Pre allocated " << *rctx << " for session=" << pSess->sessHandle;

            VLOG(2) << "    request to page out ticket " << victim << " of usage " << usage;
            // request the session to do paging
            released += pSess->pagingCb.volunteer(victim, std::move(rctx));
            if (released > 0) {
                // someone freed some memory on GPU, we are good to go.
                VLOG(2) << "    released " << released << " bytes via paging";
                return true;
            }
            VLOG(2) << "    failed";
        }
        // continue to next session
    }

    LOG(ERROR) << "All paging request failed. Dump all session usage";
    for (auto [usage, pSess] : candidates) {
        LOG(ERROR) << "Session " << pSess.get()->sessHandle << " usage: " << usage;
    }
    LOG(ERROR) << "Dump resource monitor status: " << m_resMonitor.DebugString();

    // Forcely kill one session
    for (auto [usage, pSess] : candidates) {
        auto g = sstl::with_guard(pSess.get()->mu);
        if (!pSess.get()->pagingCb) {
            continue;
        }
        forceEvicitedSess = pSess.get()->sessHandle;

        // Don't retry anymore for OOM kernels in this session
        pSess.get()->protectOOM = false;
        pSess.get()->forceEvicted = true;

        VLOG(2) << "Force evict session: " << pSess.get()->sessHandle << " with usage " << usage;
        pSess.get()->pagingCb.forceEvicted();
        return true;
    }
    LOG(ERROR) << "Nothing to force evict";
    return false;
}

ResourceContext::ResourceContext(const ResourceContext &other, const DeviceSpec &spec)
    : resMon(other.resMon)
    , m_spec(spec)
    , m_ticket(other.m_ticket)
    , session(other.session)
    , hasStaging(false)
{
}

ResourceContext::ResourceContext(PSessionItem item, ResourceMonitor &resMon)
    : resMon(resMon)
    , m_spec()
    , m_ticket(0)
    , session(std::move(item))
    , hasStaging(false)
{
}

bool ResourceContext::initializeStaging(const DeviceSpec &spec, const Resources &res, Resources *missing)
{
    this->m_spec = spec;
    DCHECK(!hasStaging);
    if (auto maybeTicket = resMon.preAllocate(res, missing)) {
        m_ticket = *maybeTicket;
        hasStaging = true;
    }
    return hasStaging;
}

void ResourceContext::releaseStaging()
{
    if (!hasStaging) {
        return;
    }
    resMon.freeStaging(m_ticket);
    hasStaging = false;

    // clean up session tickets
    if (!resMon.hasUsage(m_ticket)) {
        removeTicketFromSession();
    }
}

void ResourceContext::removeTicketFromSession() const
{
    // last resource freed
    session->removeMemoryAllocationTicket(m_ticket);
}

ResourceContext::~ResourceContext()
{
    releaseStaging();
}

ResourceContext::OperationScope ResourceContext::alloc(ResourceType type) const
{
    OperationScope scope(*this, resMon.lock());

    auto staging = scope.proxy.queryStaging(m_ticket);
    auto num = sstl::optionalGet(staging, {type, m_spec});
    if (!num) {
        return scope;
    }

    scope.res[{type, m_spec}] = *num;
    scope.valid = scope.proxy.allocate(m_ticket, scope.res);

    return scope;
}

ResourceContext::OperationScope ResourceContext::alloc(ResourceType type, size_t num) const
{
    OperationScope scope(*this, resMon.lock());

    scope.res[{type, m_spec}] = num;
    scope.valid = scope.proxy.allocate(m_ticket, scope.res);

    return scope;
}

void ResourceContext::dealloc(ResourceType type, size_t num) const
{
    ResourceTag tag{type, m_spec};
    Resources res{{tag, num}};

    resMon.free(m_ticket, res);
    session->resourceUsage(tag) -= num;
}

void ResourceContext::OperationScope::rollback()
{
    DCHECK(valid);
    proxy.free(context.ticket(), res);
}

void ResourceContext::OperationScope::commit()
{
    if (!valid) {
        return;
    }

    // the allocation is used by the session (i.e. the session left the scope without rollback)
    for (auto p : res) {
        context.session->resourceUsage(p.first) += p.second;
        context.session->notifyMemoryAllocation(context.ticket());
    }
}

std::ostream &operator<<(std::ostream &os, const ResourceContext &c)
{
    if (c.ticket() == 0) {
        return os << "AllocationTicket(Invalid)";
    }
    return os << "AllocationTicket(" << c.ticket() << ", device=" << c.spec() << ")";
}
