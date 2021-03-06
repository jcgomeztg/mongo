/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "data_replicator.h"

#include <algorithm>
#include <boost/thread.hpp>
#include <thread>

#include "mongo/base/status.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/collection_cloner.h"
#include "mongo/db/repl/database_cloner.h"
#include "mongo/db/repl/optime.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/queue.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/stacktrace.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace repl {

    // Failpoint for initial sync
    MONGO_FP_DECLARE(failInitialSyncWithBadHost);

namespace {
    size_t getSize(const BSONObj& o) {
        // SERVER-9808 Avoid Fortify complaint about implicit signed->unsigned conversion
        return static_cast<size_t>(o.objsize());
    }

    std::string toString(DataReplicatorState s) {
        switch (s) {
            case DataReplicatorState::InitialSync:
                return "InitialSync";
            case DataReplicatorState::Rollback:
                return "Rollback";
            case DataReplicatorState::Steady:
                return "Steady Replication";
            case DataReplicatorState::Uninitialized:
                return "Uninitialized";
            default:
                return "<invalid>";
        }
    }

    Timestamp findCommonPoint(HostAndPort host, Timestamp start) {
        // TODO: walk back in the oplog looking for a known/shared optime.
        return Timestamp();
    }

    bool _didRollback(HostAndPort host) {
        // TODO: rollback here, report if done.
        return false;
    }
} // namespace

    /**
     * Follows the fetcher pattern for a find+getmore
     */
    class QueryFetcher {
        MONGO_DISALLOW_COPYING(QueryFetcher);
    public:
        using CallbackFn = stdx::function<void (const BatchDataStatus&, NextAction*)>;

        QueryFetcher(ReplicationExecutor* exec,
                     const HostAndPort& source,
                     const NamespaceString& nss,
                     const BSONObj& cmdBSON,
                     const QueryFetcher::CallbackFn& onBatchAvailable);
        virtual ~QueryFetcher() = default;

        bool isActive() const { return _fetcher.isActive(); }
        Status schedule() { return _fetcher.schedule(); }
        void cancel() { return _fetcher.cancel(); }
        void wait() { if (_fetcher.isActive()) _fetcher.wait(); }
        std::string toString() const;

    protected:
        void _onFetchCallback(const BatchDataStatus& fetchResult,
                              Fetcher::NextAction* nextAction,
                              BSONObjBuilder* getMoreBob);

        virtual void _delegateCallback(const BatchDataStatus& fetchResult,
                                       NextAction* nextAction) {
            _work(fetchResult, nextAction);
        };

        ReplicationExecutor* _exec;
        Fetcher _fetcher;
        int _responses;
        const QueryFetcher::CallbackFn _work;
    };

    /**
     * Follows the fetcher pattern for a find+getmore on an oplog
     * Returns additional errors if the start oplog entry cannot be found.
     */
    class OplogFetcher : public QueryFetcher {
        MONGO_DISALLOW_COPYING(OplogFetcher);
    public:
        OplogFetcher(ReplicationExecutor* exec,
                     const Timestamp& startTS,
                     const HostAndPort& src,
                     const NamespaceString& nss,
                     const QueryFetcher::CallbackFn& work);

        virtual ~OplogFetcher() = default;
        std::string toString() const;

        const Timestamp getStartTimestamp() const {
            return _startTS;
        }

    protected:

        void _delegateCallback(const BatchDataStatus& fetchResult,
                               NextAction* nextAction);

        const Timestamp _startTS;
    };

    // QueryFetcher
    QueryFetcher::QueryFetcher(ReplicationExecutor* exec,
                               const HostAndPort& src,
                               const NamespaceString& nss,
                               const BSONObj& cmdBSON,
                               const CallbackFn& work)
          : _exec(exec),
            _fetcher(exec,
                     src,
                     nss.db().toString(),
                     cmdBSON,
                     stdx::bind(&QueryFetcher::_onFetchCallback,
                                this,
                                stdx::placeholders::_1,
                                stdx::placeholders::_2,
                                stdx::placeholders::_3)),
             _responses(0),
             _work(work) {
    }

    void QueryFetcher::_onFetchCallback(const BatchDataStatus& fetchResult,
                                        Fetcher::NextAction* nextAction,
                                        BSONObjBuilder* getMoreBob) {
                ++_responses;

                _delegateCallback(fetchResult, nextAction);
                // The fetcher will continue to call with kGetMore until an error or the last batch.
                if (fetchResult.isOK() && *nextAction == NextAction::kGetMore) {
                    const auto batchData(fetchResult.getValue());
                    invariant(getMoreBob);
                    getMoreBob->append("getMore", batchData.cursorId);
                    getMoreBob->append("collection", batchData.nss.coll());
                }
            }

    std::string QueryFetcher::toString() const {
        return str::stream() << "QueryFetcher -"
                             << " responses: " << _responses
                             << " fetcher: " << _fetcher.getDiagnosticString();
    }

    // OplogFetcher
    OplogFetcher::OplogFetcher(ReplicationExecutor* exec,
                               const Timestamp& startTS,
                               const HostAndPort& src,
                               const NamespaceString& oplogNSS,
                               const QueryFetcher::CallbackFn& work)
          // TODO: add query options await_data, oplog_replay
          : QueryFetcher(exec,
                         src,
                         oplogNSS,
                         BSON("find" << oplogNSS.coll() <<
                              "query" << BSON("ts" << BSON("$gte" << startTS))),
                         work),
            _startTS(startTS) {
    }

    std::string OplogFetcher::toString() const {
        return str::stream() << "OplogReader -"
                             << " startTS: " << _startTS.toString()
                             << " responses: " << _responses
                             << " fetcher: " << _fetcher.getDiagnosticString();
    }

    void OplogFetcher::_delegateCallback(const BatchDataStatus& fetchResult,
                                         Fetcher::NextAction* nextAction) {
        invariant(_exec->isRunThread());
        const bool checkStartTS = _responses == 0;

        if (fetchResult.isOK()) {
            Fetcher::Documents::const_iterator firstDoc = fetchResult.getValue().documents.begin();
            auto hasDoc = firstDoc != fetchResult.getValue().documents.end();

            if (checkStartTS &&
                    (!hasDoc || (hasDoc && (*firstDoc)["ts"].timestamp() != _startTS))) {
                // Set next action to none.
                *nextAction = Fetcher::NextAction::kNoAction;
                _work(Status(ErrorCodes::OplogStartMissing,
                             str::stream() << "First returned " << (*firstDoc)["ts"]
                                           << " is not where we wanted to start: "
                                           << _startTS.toString()),
                      nextAction);
                return;
            }

            if (hasDoc) {
                _work(fetchResult, nextAction);
            }
            else {
            }
        }
        else {
            _work(fetchResult, nextAction);
        }
    };

    class DatabasesCloner {
    public:
        DatabasesCloner(ReplicationExecutor* exec,
                        HostAndPort source,
                        stdx::function<void (const Status&)> finishFn)
                : _status(ErrorCodes::NotYetInitialized, ""),
                  _exec(exec),
                  _source(source),
                  _active(false),
                  _clonersActive(0),
                  _finishFn(finishFn) {
            if (!_finishFn) {
                _status = Status(ErrorCodes::InvalidOptions, "finishFn is not callable.");
            }
        };

        Status start();

        bool isActive() {
            return _active;
        }

        Status getStatus() {
            return _status;
        }

        void cancel() {
            if (!_active)
                return;
            _active = false;
            // TODO: cancel all cloners
            _setStatus(Status(ErrorCodes::CallbackCanceled, "Initial Sync Cancelled."));
        }

        void wait() {
            // TODO: wait on all cloners
        }

        std::string toString() {
            return str::stream() << "initial sync --" <<
                        " active:" << _active <<
                        " status:" << _status.toString() <<
                        " source:" << _source.toString() <<
                        " db cloners active:" << _clonersActive <<
                        " db count:" << _databaseCloners.size();
        }


        // For testing
        void setStorageInterface(CollectionCloner::StorageInterface* si) {
            _storage = si;
        }

    private:

        /**
         * Does the next action necessary for the initial sync process.
         *
         * NOTE: If (!_status.isOK() || !_isActive) then early return.
         */
        void _doNextActions();

        /**
         *  Setting the status to not-OK will stop the process
         */
        void _setStatus(CBHStatus s) {
            _setStatus(s.getStatus());
        }

        /**
         *  Setting the status to not-OK will stop the process
         */
        void _setStatus(Status s) {
            // Only set the first time called, all subsequent failures are not recorded --only first
            if (_status.code() != ErrorCodes::NotYetInitialized) {
                _status = s;
            }
        }

        /**
         *  Setting the status to not-OK will stop the process
         */
        void _setStatus(TimestampStatus s) {
            _setStatus(s.getStatus());
        }

        void _failed();

        /** Called each time a database clone is finished */
        void _onEachDBCloneFinish(const Status& status, const std::string name);

        //  Callbacks

        void _onListDatabaseFinish(const CommandCallbackArgs& cbd);


        // Member variables
        Status _status; // If it is not OK, we stop everything.
        ReplicationExecutor* _exec; // executor to schedule things with
        HostAndPort _source; // The source to use, until we get an error
        bool _active; // false until we start
        std::vector<std::shared_ptr<DatabaseCloner>> _databaseCloners; // database cloners by name
        int _clonersActive;

        const stdx::function<void (const Status&)> _finishFn;

        CollectionCloner::StorageInterface* _storage;
    };

    /** State held during Initial Sync */
    struct InitialSyncState {
        InitialSyncState(DatabasesCloner cloner, Event event)
            : dbsCloner(cloner), finishEvent(event), status(ErrorCodes::IllegalOperation, "")  {};

        DatabasesCloner dbsCloner; // Cloner for all databases included in initial sync.
        Timestamp beginTimestamp; // Timestamp from the latest entry in oplog when started.
        Timestamp stopTimestamp; // Referred to as minvalid, or the place we can transition states.
        Event finishEvent; // event fired on completion, either successful or not.
        Status status; // final status, only valid after the finishEvent fires.
        size_t fetchedMissingDocs;
        size_t appliedOps;

        // Temporary fetch for things like fetching remote optime, or tail
        std::unique_ptr<Fetcher> tmpFetcher;
        TimestampStatus getLatestOplogTimestamp(ReplicationExecutor* exec,
                                                HostAndPort source,
                                                const NamespaceString& oplogNS);
        void setStatus(const Status& s);
        void setStatus(const CBHStatus& s);
        void _setTimestampSatus(const BatchDataStatus& fetchResult,
                                Fetcher::NextAction* nextAction,
                                TimestampStatus* status) ;
    };

    // Initial Sync state
    TimestampStatus InitialSyncState::getLatestOplogTimestamp(ReplicationExecutor* exec,
                                                              HostAndPort source,
                                                              const NamespaceString& oplogNS) {

        BSONObj query = BSON("find" << oplogNS.coll() <<
                             "sort" << BSON ("$natural" << -1) <<
                             "limit" << 1);

        TimestampStatus timestampStatus(ErrorCodes::BadValue, "");
        Fetcher f(exec,
                  source,
                  oplogNS.db().toString(),
                  query,
                  stdx::bind(&InitialSyncState::_setTimestampSatus, this, stdx::placeholders::_1,
                             stdx::placeholders::_2, &timestampStatus));
        Status s = f.schedule();
        if (!s.isOK()) {
            return TimestampStatus(s);
        }

        // wait for fetcher to get the oplog position.
        f.wait();
        return timestampStatus;
    }

    void InitialSyncState::_setTimestampSatus(const BatchDataStatus& fetchResult,
                                          Fetcher::NextAction* nextAction,
                                          TimestampStatus* status) {
        if (!fetchResult.isOK()) {
            *status = TimestampStatus(fetchResult.getStatus());
        } else {
            // TODO: Set _beginTimestamp from first doc "ts" field.
            const auto docs = fetchResult.getValue().documents;
            const auto hasDoc = docs.begin() != docs.end();
            if (!hasDoc || !docs.begin()->hasField("ts")) {
                *status = TimestampStatus(ErrorCodes::FailedToParse,
                                          "Could not find an oplog entry with 'ts' field.");
            } else {
                *status = TimestampStatus(docs.begin()->getField("ts").timestamp());
            }
        }
    }

    void InitialSyncState::setStatus(const Status& s) {
        status = s;
    }
    void InitialSyncState::setStatus(const CBHStatus& s) {
        setStatus(s.getStatus());
    }

    // Initial Sync
    Status DatabasesCloner::start() {
        _active = true;

        if (!_status.isOK() && _status.code() != ErrorCodes::NotYetInitialized) {
            return _status;
        }

        _status = Status::OK();

        log() << "starting cloning of all databases";
        // Schedule listDatabase command which will kick off the database cloner per result db.
        Request listDBsReq(_source, "admin", BSON("listDatabases" << true));
        CBHStatus s = _exec->scheduleRemoteCommand(
                                listDBsReq,
                                stdx::bind(&DatabasesCloner::_onListDatabaseFinish,
                                           this,
                                           stdx::placeholders::_1));
        if (!s.isOK()) {
            _setStatus(s);
            _failed();
        }

        _doNextActions();

        return _status;
    }

    void DatabasesCloner::_onListDatabaseFinish(const CommandCallbackArgs& cbd) {
        invariant(_exec->isRunThread());
        const Status respStatus = cbd.response.getStatus();
        if (!respStatus.isOK()) {
            // TODO: retry internally?
            _setStatus(respStatus);
            _doNextActions();
            return;
        }

        const auto respBSON = cbd.response.getValue().data;

        // There should not be any cloners yet
        invariant(_databaseCloners.size() == 0);

        const auto okElem = respBSON["ok"];
        if (okElem.trueValue()) {
            const auto dbsElem = respBSON["databases"].Obj();
            BSONForEach(arrayElement, dbsElem) {
                const BSONObj dbBSON = arrayElement.Obj();
                const std::string name = dbBSON["name"].str();
                ++_clonersActive;
                std::shared_ptr<DatabaseCloner> dbCloner{nullptr};
                try {
                    dbCloner.reset(new DatabaseCloner(
                                         _exec,
                                         _source,
                                         name,
                                         BSONObj(), // do not filter database out.
                                         [](const BSONObj&) { return true; }, // clone all dbs.
                                         _storage, // use storage provided.
                                         [](const Status& status, const NamespaceString& srcNss) {
                                             if (status.isOK()) {
                                                 log() << "collection clone finished: " << srcNss;
                                             }
                                             else {
                                                 log() << "collection clone for '"
                                                       << srcNss << "' failed due to "
                                                       << status.toString();
                                             }
                                         },
                                         [=](const Status& status) {
                                             _onEachDBCloneFinish(status, name);
                                         }));
                }
                catch (...) {
                    // error creating, fails below.
                }

                Status s = dbCloner ? dbCloner->start() : Status(ErrorCodes::UnknownError, "Bad!");

                if (!s.isOK()) {
                    std::string err = str::stream() << "could not create cloner for database: "
                                                    << name << " due to: " << s.toString();
                    _setStatus(Status(ErrorCodes::InitialSyncFailure, err));
                    error() << err;
                    break; // exit for_each loop
                }

                // add cloner to list.
                _databaseCloners.push_back(dbCloner);
            }
        }
        else {
            _setStatus(Status(ErrorCodes::InitialSyncFailure,
                              "failed to clone databases due to failed server response."));
        }

        // Move on to the next steps in the process.
        _doNextActions();
    }

    void DatabasesCloner::_onEachDBCloneFinish(const Status& status, const std::string name) {
        auto clonersLeft = --_clonersActive;

        if (status.isOK()) {
            log() << "database clone finished: " << name;
        }
        else {
            log() << "database clone failed due to "
                  << status.toString();
            _setStatus(status);
        }

        if (clonersLeft == 0) {
            _active = false;
            // All cloners are done, trigger event.
            log() << "all database clones finished, calling _finishFn";
            _finishFn(_status);
        }

        _doNextActions();
    }

    void DatabasesCloner::_doNextActions() {
        // If we are no longer active or we had an error, stop doing more
        if (!(_active && _status.isOK())) {
            if (!_status.isOK()) {
                // trigger failed state
                _failed();
            }
            return;
        }
    }

    void DatabasesCloner::_failed() {
        // TODO: cancel outstanding work, like any cloners active
        invariant(_finishFn);
        _finishFn(_status);
    }

    // Data Replicator
    DataReplicator::DataReplicator(DataReplicatorOptions opts,
                                   ReplicationExecutor* exec,
                                   ReplicationCoordinator* replCoord)
                            : _opts(opts),
                              _exec(exec),
                              _replCoord(replCoord),
                              _state(DataReplicatorState::Uninitialized),
                              _fetcherPaused(false),
                              _reporterPaused(false),
                              _applierActive(false),
                              _applierPaused(false),
                              _oplogBuffer(256*1024*1024, &getSize), // Limit buffer to 256MB
                              _doShutdown(false) {
        // TODO: replace this with a method in the replication coordinator.
        if (replCoord) {
            _batchCompletedFn = [&] (const Timestamp& ts) {
                OpTime ot(ts, 0);
                _replCoord->setMyLastOptime(ot);
            };
        }
        else {
            _batchCompletedFn = [] (const Timestamp& ts) {
            };
        }
    }

    DataReplicator::DataReplicator(DataReplicatorOptions opts,
                                   ReplicationExecutor* exec)
                            : DataReplicator(opts, exec, nullptr) {
    }

    DataReplicator::~DataReplicator() {
        DESTRUCTOR_GUARD(
            _cancelAllHandles_inlock();
            _waitOnAll_inlock();
        );
    }

    Status DataReplicator::start() {
        UniqueLock lk(_mutex);
        if (_state != DataReplicatorState::Uninitialized) {
            return Status(ErrorCodes::IllegalOperation,
                          str::stream() << "Already started in another state: "
                                        << toString(_state));
        }

        _state = DataReplicatorState::Steady;
        _applierPaused = false;
        _fetcherPaused = false;
        _reporterPaused = false;
        _doNextActions_Steady_inlock();
        return Status::OK();
    }

    Status DataReplicator::shutdown() {
        return _shutdown();
    }

    Status DataReplicator::pause() {
        _pauseApplier();
        return Status::OK();
    }

    std::string DataReplicator::getDiagnosticString() const {
        str::stream out;
        out << "DataReplicator -"
            << " opts: " << _opts.toString()
            << " oplogFetcher: " << _fetcher->toString()
            << " opsBuffered: " << _oplogBuffer.size()
            << " state: " << toString(_state);
        switch (_state) {
            case DataReplicatorState::InitialSync:
                out << " opsAppied: " << _initialSyncState->appliedOps
                    << " status: " << _initialSyncState->status.toString();
                break;
            case DataReplicatorState::Steady:
                // TODO: add more here
                break;
            case DataReplicatorState::Rollback:
                // TODO: add more here
                break;
            default:
                break;
        }

        return out;
    }

    Status DataReplicator::resume(bool wait) {
        CBHStatus handle = _exec->scheduleWork(stdx::bind(&DataReplicator::_resumeFinish,
                                               this,
                                               stdx::placeholders::_1));
        const Status status = handle.getStatus();
        if (wait && status.isOK()) {
            _exec->wait(handle.getValue());
        }
        return status;
    }

    void DataReplicator::_resumeFinish(CallbackArgs cbData) {
        UniqueLock lk(_mutex);
        _fetcherPaused = _applierPaused = false;
        lk.unlock();

        _doNextActions();
    }

    void DataReplicator::_pauseApplier() {
        LockGuard lk(_mutex);
        if (_applier)
            _applier->wait();
        _applierPaused = true;
        _applier.reset();
    }

    Timestamp DataReplicator::_applyUntil(Timestamp untilTimestamp) {
        // TODO: block until all oplog buffer application is done to the given optime
        return Timestamp();
    }

    Timestamp DataReplicator::_applyUntilAndPause(Timestamp untilTimestamp) {
        //_run(&_pauseApplier);
        return _applyUntil(untilTimestamp);
    }

    TimestampStatus DataReplicator::flushAndPause() {
        //_run(&_pauseApplier);
        UniqueLock lk(_mutex);
        if (_applierActive) {
            _applierPaused = true;
            lk.unlock();
            _applier->wait();
            lk.lock();
        }
        return TimestampStatus(_lastTimestampApplied);
    }

    void DataReplicator::_resetState_inlock(Timestamp lastAppliedOptime) {
        invariant(!_anyActiveHandles_inlock());
        _lastTimestampApplied = _lastTimestampFetched = lastAppliedOptime;
         _oplogBuffer.clear();
     }

    void DataReplicator::slavesHaveProgressed() {
        if (_reporter) {
            _reporter->trigger();
        }
    }

    void DataReplicator::_setInitialSyncStorageInterface(CollectionCloner::StorageInterface* si) {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _storage = si;
        if (_initialSyncState) {
            _initialSyncState->dbsCloner.setStorageInterface(_storage);
        }
    }

    TimestampStatus DataReplicator::resync() {
        _shutdown();
        // Drop databases and do initialSync();
        CBHStatus cbh = _exec->scheduleDBWork([&](const CallbackArgs& cbData) {
            _storage->dropUserDatabases(cbData.txn);
        });

        if (!cbh.isOK()) {
            return TimestampStatus(cbh.getStatus());
        }

        _exec->wait(cbh.getValue());

        TimestampStatus status = initialSync();
        if (status.isOK()) {
            _resetState_inlock(status.getValue());
        }
        return status;
    }

    TimestampStatus DataReplicator::initialSync() {
        Timer t;
        UniqueLock lk(_mutex);
        if (_state != DataReplicatorState::Uninitialized) {
            if (_state == DataReplicatorState::InitialSync)
                return TimestampStatus(ErrorCodes::InvalidRoleModification,
                                       (str::stream() << "Already doing initial sync;try resync"));
            else {
                return TimestampStatus(ErrorCodes::AlreadyInitialized,
                                       (str::stream() << "Cannot do initial sync in "
                                                      << toString(_state) << " state."));
            }
        }

        _state = DataReplicatorState::InitialSync;

        // The reporter is paused for the duration of the initial sync, so cancel just in case.
        if (_reporter) {
            _reporter->cancel();
        }
        _reporterPaused = true;
        _applierPaused = true;

        // TODO: set minvalid doc initial sync state.

        const int maxFailedAttempts = 10;
        int failedAttempts = 0;
        Status attemptErrorStatus(Status::OK());
        while (failedAttempts < maxFailedAttempts) {
            // For testing, we may want to fail if we receive a getmore.
            if (MONGO_FAIL_POINT(failInitialSyncWithBadHost)) {
                attemptErrorStatus = Status(ErrorCodes::InvalidSyncSource, "no sync source avail.");
            }

            Event initialSyncFinishEvent;
            if (attemptErrorStatus.isOK() && _syncSource.empty()) {
                attemptErrorStatus = _ensureGoodSyncSource_inlock();
            }
            else if(attemptErrorStatus.isOK()) {
                StatusWith<Event> status = _exec->makeEvent();
                if (!status.isOK()) {
                    attemptErrorStatus = status.getStatus();
                } else {
                    initialSyncFinishEvent = status.getValue();
                }
            }

            if (attemptErrorStatus.isOK()) {
                invariant(initialSyncFinishEvent.isValid());
                _initialSyncState.reset(new InitialSyncState(
                                 DatabasesCloner(_exec,
                                                 _syncSource,
                                                 stdx::bind(&DataReplicator::_onDataClonerFinish,
                                                            this,
                                                            stdx::placeholders::_1)),
                                 initialSyncFinishEvent));

                _initialSyncState->dbsCloner.setStorageInterface(_storage);
                const NamespaceString ns(_opts.remoteOplogNS);
                TimestampStatus tsStatus = _initialSyncState->getLatestOplogTimestamp(
                                                                                _exec,
                                                                                _syncSource,
                                                                                ns);
                attemptErrorStatus = tsStatus.getStatus();
                if (attemptErrorStatus.isOK()) {
                    _initialSyncState->beginTimestamp = tsStatus.getValue();
                    _fetcher.reset(new OplogFetcher(_exec,
                                                    _initialSyncState->beginTimestamp,
                                                    _syncSource,
                                                    _opts.remoteOplogNS,
                                                    stdx::bind(&DataReplicator::_onOplogFetchFinish,
                                                               this,
                                                               stdx::placeholders::_1,
                                                               stdx::placeholders::_2)));
                    _scheduleFetch_inlock();
                    lk.unlock();
                    _initialSyncState->dbsCloner.start(); // When the cloner is done applier starts.
                    invariant(_initialSyncState->finishEvent.isValid());
                    _exec->waitForEvent(_initialSyncState->finishEvent);
                    attemptErrorStatus = _initialSyncState->status;

                    // Re-lock DataReplicator Internals
                    lk.lock();
                }
            }

            if (attemptErrorStatus.isOK()) {
                break; // success
            }

            ++failedAttempts;

            error() << "Initial sync attempt failed -- attempts left: "
                    << (maxFailedAttempts - failedAttempts) << " cause: "
                    << attemptErrorStatus;

            // Sleep for retry time
            lk.unlock();
            sleepmillis(_opts.initialSyncRetryWait.count());
            lk.lock();

            // No need to print a stack
            if (failedAttempts >= maxFailedAttempts) {
                const std::string err = "The maximum number of retries"
                                       " have been exhausted for initial sync.";
                severe() << err;
                return Status(ErrorCodes::InitialSyncFailure, err);
            }
        }

        // Success, cleanup
        // TODO: re-enable, find blocking call from tests
/*
        _cancelAllHandles_inlock();
        _waitOnAll_inlock();

        _reporterPaused = false;
        _fetcherPaused = false;
        _fetcher.reset(nullptr);
        _tmpFetcher.reset(nullptr);
        _applierPaused = false;
        _applier.reset(nullptr);
        _applierActive = false;
        _initialSyncState.reset(nullptr);
        _oplogBuffer.clear();
        _resetState_inlock(_lastTimestampApplied);
*/
        log() << "Initial sync took: " << t.millis() << " milliseconds.";
        return TimestampStatus(_lastTimestampApplied);
    }

    void DataReplicator::_onDataClonerFinish(const Status& status) {
        log() << "data clone finished, status: " << status.toString();
        if (!status.isOK()) {
            // Iniitial sync failed during cloning of databases
            _initialSyncState->setStatus(status);
            _exec->signalEvent(_initialSyncState->finishEvent);
            return;
        }

        BSONObj query = BSON("find" << _opts.remoteOplogNS.coll() <<
                             "sort" << BSON ("$natural" << -1) <<
                             "limit" << 1);

        TimestampStatus timestampStatus(ErrorCodes::BadValue, "");
        _tmpFetcher.reset(new QueryFetcher(_exec,
                                           _syncSource,
                                           _opts.remoteOplogNS,
                                           query,
                                           stdx::bind(&DataReplicator::_onApplierReadyStart,
                                                      this,
                                                      stdx::placeholders::_1,
                                                      stdx::placeholders::_2)));
        Status s = _tmpFetcher->schedule();
        if (!s.isOK()) {
            _initialSyncState->setStatus(s);
        }
    }

    void DataReplicator::_onApplierReadyStart(const BatchDataStatus& fetchResult,
                                              NextAction* nextAction) {
        invariant(_exec->isRunThread());
        // Data clone done, move onto apply.
        TimestampStatus ts(ErrorCodes::OplogStartMissing, "");
        _initialSyncState->_setTimestampSatus(fetchResult, nextAction, &ts);
        if (ts.isOK()) {
            // TODO: set minvalid?
            LockGuard lk(_mutex);
            _initialSyncState->stopTimestamp = ts.getValue();
            if (_lastTimestampApplied < ts.getValue()) {
                log() << "waiting for applier to run until ts: " << ts.getValue();
            }
            invariant(_applierPaused);
            _applierPaused = false;
            _doNextActions_InitialSync_inlock();
        }
        else {
           _initialSyncState->setStatus(ts.getStatus());
           _doNextActions();
        }
    }

    bool DataReplicator::_anyActiveHandles_inlock() const {
        return _applierActive ||
               (_fetcher && _fetcher->isActive()) ||
               (_initialSyncState && _initialSyncState->dbsCloner.isActive()) ||
               (_reporter && _reporter->isActive());
    }

    void DataReplicator::_cancelAllHandles_inlock() {
        if (_fetcher)
            _fetcher->cancel();
        if (_applier)
            _applier->cancel();
        if (_reporter)
            _reporter->cancel();
        if (_initialSyncState && _initialSyncState->dbsCloner.isActive())
            _initialSyncState->dbsCloner.cancel();
    }

    void DataReplicator::_waitOnAll_inlock() {
        if (_fetcher)
            _fetcher->wait();
        if (_applier)
            _applier->wait();
        if (_reporter)
            _reporter->wait();
        if (_initialSyncState)
            _initialSyncState->dbsCloner.wait();
    }

    void DataReplicator::_doNextActions() {
        // Can be in one of 3 main states/modes (DataReplicatiorState):
        // 1.) Initial Sync
        // 2.) Rollback
        // 3.) Steady (Replication)

        // Check for shutdown flag, signal event
        LockGuard lk(_mutex);
        if (_doShutdown) {
            if(!_anyActiveHandles_inlock()) {
                _exec->signalEvent(_onShutdown);
            }
            return;
        }

        // Do work for the current state
        switch (_state) {
            case DataReplicatorState::Rollback:
                _doNextActions_Rollback_inlock();
                break;
            case DataReplicatorState::InitialSync:
                _doNextActions_InitialSync_inlock();
                break;
            case DataReplicatorState::Steady:
                _doNextActions_Steady_inlock();
                break;
            default:
                return;
        }

        // transition when needed
        _changeStateIfNeeded();
    }

    void DataReplicator::_doNextActions_InitialSync_inlock() {
        if (!_initialSyncState) {
            // TODO: Error case?, reset to uninit'd
            _state = DataReplicatorState::Uninitialized;
            log() << "_initialSyncState, so resetting state to Uninitialized";
            return;
        }

        if (!_initialSyncState->dbsCloner.isActive()) {
            if (!_initialSyncState->dbsCloner.getStatus().isOK()) {
                // TODO: Initial sync failed
            }
            else {
                if (!_lastTimestampApplied.isNull() &&
                                        _lastTimestampApplied >= _initialSyncState->stopTimestamp) {
                    invariant(_initialSyncState->finishEvent.isValid());
                    log() << "Applier done, initial sync done, end timestamp: "
                          << _initialSyncState->stopTimestamp << " , last applier: "
                          << _lastTimestampApplied;
                    _state = DataReplicatorState::Uninitialized;
                    _initialSyncState->setStatus(Status::OK());
                    _exec->signalEvent(_initialSyncState->finishEvent);
                }
                else {
                    // Run steady state events to fetch/apply.
                    _doNextActions_Steady_inlock();
                }
            }
        }
    }

    void DataReplicator::_doNextActions_Rollback_inlock() {
        // TODO: check rollback state and do next actions
        // move from rollback phase to rollback phase via scheduled work in exec
    }

    void DataReplicator::_doNextActions_Steady_inlock() {
        // Check sync source is still good.
        if (_syncSource.empty()) {
            _syncSource = _replCoord->chooseNewSyncSource();
        }
        if (_syncSource.empty()) {
            // No sync source, reschedule check
            Date_t when = _exec->now() + _opts.syncSourceRetryWait;
            // schedule self-callback w/executor
            _exec->scheduleWorkAt(when, // to try to get a new sync source in a bit
                                  [this] (const CallbackArgs&) { _doNextActions(); });
        } else {
            // Check if active fetch, if not start one
            if (!_fetcher || !_fetcher->isActive()) {
                _scheduleFetch_inlock();
            }
        }

        // Check if no active apply and ops to apply
        if (!_applierActive && _oplogBuffer.size()) {
            _scheduleApplyBatch_inlock();
        }

        if (!_reporterPaused && (!_reporter || !_reporter->getStatus().isOK())) {
            // TODO get reporter in good shape
            _reporter.reset(new Reporter(_exec, _replCoord, _syncSource));
        }
    }

    Operations DataReplicator::_getNextApplierBatch_inlock() {
        // Return a new batch of ops to apply.
        // TODO: limit the batch like SyncTail::tryPopAndWaitForMore
        Operations ops;
        BSONObj op;
        while(_oplogBuffer.tryPop(op)) {
            ops.push_back(op);
        }
        return ops;
    }

    void DataReplicator::_onApplyBatchFinish(const CallbackArgs& cbData,
                                             const TimestampStatus& ts,
                                             const Operations& ops,
                                             const size_t numApplied) {
        invariant(cbData.status.isOK());
        UniqueLock lk(_mutex);
        _initialSyncState->appliedOps += numApplied;
        if (!ts.isOK()) {
            _handleFailedApplyBatch(ts, ops);
            return;
        }

        _applierActive = false;
        _lastTimestampApplied = ts.getValue();
        lk.unlock();

        if (_batchCompletedFn) {
            _batchCompletedFn(ts.getValue());
        }
        // TODO: move the reported to the replication coordinator and set _batchCompletedFn to a
        // function in the replCoord.
        if (_reporter) {
            _reporter->trigger();
        }

        _doNextActions();
    }

    void DataReplicator::_handleFailedApplyBatch(const TimestampStatus& ts, const Operations& ops) {
        switch (_state) {
            case DataReplicatorState::InitialSync:
                // TODO: fetch missing doc, and retry.
                _scheduleApplyAfterFetch(ops);
                break;
            case DataReplicatorState::Rollback:
                // TODO: nothing?
            default:
                // fatal
                fassert(28666, ts.getStatus());
        }
    }

    void DataReplicator::_scheduleApplyAfterFetch(const Operations& ops) {
        ++_initialSyncState->fetchedMissingDocs;
        // TODO: check collection.isCapped, like SyncTail::getMissingDoc
        const BSONObj failedOplogEntry = *ops.begin();
        const BSONElement missingIdElem = failedOplogEntry.getFieldDotted("o2._id");
        const NamespaceString nss(ops.begin()->getField("ns").str());
        const BSONObj query = BSON("find" << nss.coll() << "query" << missingIdElem.wrap());
        _tmpFetcher.reset(new QueryFetcher(_exec, _syncSource, nss, query,
                                           stdx::bind(&DataReplicator::_onMissingFetched,
                                                      this,
                                                      stdx::placeholders::_1,
                                                      stdx::placeholders::_2,
                                                      ops,
                                                      nss)));
        Status s = _tmpFetcher->schedule();
        if (!s.isOK()) {
            // record error and take next step based on it.
            _initialSyncState->setStatus(s);
            _doNextActions();
        }
    }

    void DataReplicator::_onMissingFetched(const BatchDataStatus& fetchResult,
                                           Fetcher::NextAction* nextAction,
                                           const Operations& ops,
                                           const NamespaceString nss) {
        invariant(_exec->isRunThread());

        if (!fetchResult.isOK()) {
            // TODO: do retries on network issues, like SyncTail::getMissingDoc
            _initialSyncState->setStatus(fetchResult.getStatus());
            _doNextActions();
            return;
        } else if (!fetchResult.getValue().documents.size()) {
            // TODO: skip apply for this doc, like multiInitialSyncApply?
            _initialSyncState->setStatus(Status(ErrorCodes::InitialSyncFailure,
                                                "missing doc not found"));
            _doNextActions();
            return;
        }

        const BSONObj missingDoc = *fetchResult.getValue().documents.begin();
        Status rs{Status::OK()};
        auto s = _exec->scheduleDBWork(([&](const CallbackArgs& cd) {
                                           rs = _storage->insertMissingDoc(cd.txn, nss, missingDoc);
                                       }),
                                       nss,
                                       MODE_IX);
        if (!s.isOK()) {
            _initialSyncState->setStatus(s);
            _doNextActions();
            return;
        }

        _exec->wait(s.getValue());
        if (!rs.isOK()) {
            _initialSyncState->setStatus(rs);
            _doNextActions();
            return;
        }

        auto status = _scheduleApplyBatch_inlock(ops);
        if (!status.isOK()) {
            LockGuard lk(_mutex);
            _initialSyncState->setStatus(status);
            _exec->signalEvent(_initialSyncState->finishEvent);
        }
    }

    Status DataReplicator::_scheduleApplyBatch() {
        LockGuard lk(_mutex);
        return _scheduleApplyBatch_inlock();
    }

    Status DataReplicator::_scheduleApplyBatch_inlock() {
        if (!_applierPaused && !_applierActive) {
            _applierActive = true;
            const Operations ops = _getNextApplierBatch_inlock();
            invariant(ops.size());
            invariant(_opts.applierFn);
            invariant(!(_applier && _applier->isActive()));
            return _scheduleApplyBatch_inlock(ops);
        }
        return Status::OK();
    }

    Status DataReplicator::_scheduleApplyBatch_inlock(const Operations& ops) {
        auto lambda = [this] (const TimestampStatus& ts, const Operations& theOps) {
            CBHStatus status = _exec->scheduleWork(stdx::bind(&DataReplicator::_onApplyBatchFinish,
                                                              this,
                                                              stdx::placeholders::_1,
                                                              ts,
                                                              theOps,
                                                              theOps.size()));
           if (!status.isOK()) {
               LockGuard lk(_mutex);
               _initialSyncState->setStatus(status);
               _exec->signalEvent(_initialSyncState->finishEvent);
               return;
           }
           // block until callback done.
           _exec->wait(status.getValue());
       };

       _applier.reset(new Applier(_exec, ops, _opts.applierFn, lambda));
       return _applier->start();
    }

    Status DataReplicator::_scheduleFetch() {
        LockGuard lk(_mutex);
        return _scheduleFetch_inlock();
    }

    Status DataReplicator::_ensureGoodSyncSource_inlock() {
        if (_syncSource.empty()) {
            if (_replCoord) {
                _syncSource = _replCoord->chooseNewSyncSource();
                if (!_syncSource.empty()) {
                    return Status::OK();
                }
            } else {
                _syncSource = _opts.syncSource; // set this back to the options source
            }

            return Status{ErrorCodes::InvalidSyncSource, "No valid sync source."};
        }
        return Status::OK();
    }

    Status DataReplicator::_scheduleFetch_inlock() {
        if (!_fetcher) {
            const auto startOptime = _replCoord ? _replCoord->getMyLastOptime().getTimestamp()
                                                : _opts.startOptime;
            if (!_ensureGoodSyncSource_inlock().isOK()) {
                auto status = _exec->scheduleWork([this](const CallbackArgs&){ _doNextActions(); });
                if (!status.isOK()) {
                    return status.getStatus();
                }
            }
            const auto remoteOplogNS = _opts.remoteOplogNS;

            // TODO: add query options await_data, oplog_replay
            _fetcher.reset(new OplogFetcher(_exec,
                                            startOptime,
                                            _syncSource,
                                            remoteOplogNS,
                                            stdx::bind(&DataReplicator::_onOplogFetchFinish,
                                                       this,
                                                       stdx::placeholders::_1,
                                                       stdx::placeholders::_2)));

        }
        if (!_fetcher->isActive()) {
            Status status = _fetcher->schedule();
            if (!status.isOK()) {
                return status;
            }
        }
        return Status::OK();
    }

    Status DataReplicator::_scheduleReport() {
        // TODO
        return Status::OK();
    }

    void DataReplicator::_changeStateIfNeeded() {
        // TODO
    }

    Status DataReplicator::_shutdown() {
        StatusWith<Event> eventStatus = _exec->makeEvent();
        if (!eventStatus.isOK()) return eventStatus.getStatus();
        UniqueLock lk(_mutex);
        _onShutdown = eventStatus.getValue();
        _cancelAllHandles_inlock();
        _doShutdown = true;
        lk.unlock();

        // Schedule _doNextActions in case nothing is active to trigger the _onShutdown event.
        StatusWith<Handle> statusHandle = _exec->scheduleWork(
                                                [this] (const CallbackArgs&) { _doNextActions(); });
        if (statusHandle.isOK()) {
            _exec->waitForEvent(_onShutdown);
        } else {
            return statusHandle.getStatus();
        }

        invariant(!_fetcher->isActive());
        invariant(!_applierActive);
        invariant(!_reporter->isActive());
        return Status::OK();
    }

    bool DataReplicator::_needToRollback(HostAndPort source, Timestamp lastApplied) {
        if ((_rollbackCommonOptime = findCommonPoint(source, lastApplied)).isNull()) {
            return false;
        } else {
            return true;
        }
    }

    void DataReplicator::_onOplogFetchFinish(const StatusWith<Fetcher::BatchData>& fetchResult,
                                             Fetcher::NextAction* nextAction) {
        invariant(_exec->isRunThread());
        const Status status = fetchResult.getStatus();
        if (status.code() == ErrorCodes::CallbackCanceled)
            return;

        if (status.isOK()) {
            const auto docs = fetchResult.getValue().documents;
            if (docs.begin() != docs.end()) {
                LockGuard lk(_mutex);
                std::for_each(docs.cbegin(),
                              docs.cend(),
                              [&](const BSONObj& doc) {
                                  _oplogBuffer.push(doc);
                              });
                auto doc = docs.rbegin();
                BSONElement tsElem(doc->getField("ts"));
                while(tsElem.eoo() && doc != docs.rend()) {
                    tsElem =  (doc++)->getField("ts");
                }

                if (!tsElem.eoo()) {
                    _lastTimestampFetched = tsElem.timestamp();
                } else {
                    warning() <<
                            "Did not find a 'ts' timestamp field in any of the fetched documents";
                }
            }
            if (*nextAction == Fetcher::NextAction::kNoAction) {
                // TODO: create new fetcher?, with new query from where we left off -- d'tor fetcher

            }
        }

        if (!status.isOK()) {
            // Got an error, now decide what to do...
            switch (status.code()) {
                case ErrorCodes::OplogStartMissing: {
                    // possible rollback
                    bool didRollback = _didRollback(_syncSource);
                    if (!didRollback) {
                        auto s = _replCoord->setFollowerMode(MemberState::RS_RECOVERING);
                        if (!s) {
                            error() << "Failed to transition to RECOVERING when "
                                       "we couldn't find oplog start position ("
                                    << _fetcher->getStartTimestamp().toString()
                                    << ") from sync source: "
                                    << _syncSource.toString();
                        }
                        Date_t until{_exec->now() +
                                     _opts.blacklistSyncSourcePenaltyForOplogStartMissing};
                        _replCoord->blacklistSyncSource(_syncSource, until);
                    }
                    else {
                        // TODO: cleanup state/restart -- set _lastApplied, and other stuff
                    }
                    break;
                }
                case ErrorCodes::InvalidSyncSource:
                    // Error, sync source
                    // fallthrough
                default:
                    Date_t until{_exec->now() +
                                 _opts.blacklistSyncSourcePenaltyForNetworkConnectionError};
                    _replCoord->blacklistSyncSource(_syncSource, until);
            }
            LockGuard lk(_mutex);
            _syncSource = HostAndPort();
        }

        _doNextActions();
    }
} // namespace repl
} // namespace mongo
