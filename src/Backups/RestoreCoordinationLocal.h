#pragma once

#include <Backups/IRestoreCoordination.h>
#include <Backups/BackupConcurrencyCheck.h>
#include <Parsers/CreateQueryUUIDs.h>
#include <Common/Logger.h>
#include <mutex>
#include <set>
#include <unordered_set>


namespace DB
{
class ASTCreateQuery;

/// Implementation of the IRestoreCoordination interface performing coordination in memory.
class RestoreCoordinationLocal : public IRestoreCoordination
{
public:
    RestoreCoordinationLocal(bool allow_concurrent_restore_, BackupConcurrencyCounters & concurrency_counters_);
    ~RestoreCoordinationLocal() override;

    void setRestoreQueryIsSentToOtherHosts() override {}
    bool isRestoreQuerySentToOtherHosts() const override { return false; }
    Strings setStage(const String &, const String &, bool) override { return {}; }
    void setError(std::exception_ptr, bool) override { is_error_set = true; }  /// RestoreStarter::onException() has already logged the error.
    bool isErrorSet() const override { return is_error_set; }
    void waitOtherHostsFinish(bool) const override {}
    void finish(bool) override { is_finished = true; }
    bool finished() const override { return is_finished; }
    bool allHostsFinished() const override { return finished(); }
    void cleanup(bool) override {}

    /// Starts creating a shared database. Returns false if there is another host which is already creating this database.
    bool acquireCreatingSharedDatabase(const String & database_name) override;

    /// Starts creating a table in a replicated database. Returns false if there is another host which is already creating this table.
    bool acquireCreatingTableInReplicatedDatabase(const String & database_zk_path, const String & table_name) override;

    /// Sets that this replica is going to restore a partition in a replicated table.
    /// The function returns false if this partition is being already restored by another replica.
    bool acquireInsertingDataIntoReplicatedTable(const String & table_zk_path) override;

    /// Sets that this replica is going to restore a ReplicatedAccessStorage.
    /// The function returns false if this access storage is being already restored by another replica.
    bool acquireReplicatedAccessStorage(const String & access_storage_zk_path) override;

    /// Sets that this replica is going to restore replicated user-defined functions.
    /// The function returns false if user-defined function at a specified zk path are being already restored by another replica.
    bool acquireReplicatedSQLObjects(const String & loader_zk_path, UserDefinedSQLObjectType object_type) override;

    /// Sets that this table is going to restore data into Keeper for all KeeperMap tables defined on root_zk_path.
    /// The function returns false if data for this specific root path is already being restored by another table.
    bool acquireInsertingDataForKeeperMap(const String & root_zk_path, const String & table_unique_id) override;

    /// Generates a new UUID for a table. The same UUID must be used for a replicated table on each replica,
    /// (because otherwise the macro "{uuid}" in the ZooKeeper path will not work correctly).
    void generateUUIDForTable(ASTCreateQuery & create_query) override;

    ZooKeeperRetriesInfo getOnClusterInitializationKeeperRetriesInfo() const override;

private:
    LoggerPtr const log;
    BackupConcurrencyCheck concurrency_check;

    std::set<std::pair<String /* database_zk_path */, String /* table_name */>> acquired_tables_in_replicated_databases TSA_GUARDED_BY(mutex);
    std::unordered_set<String /* table_zk_path */> acquired_data_in_replicated_tables TSA_GUARDED_BY(mutex);
    std::unordered_map<String, CreateQueryUUIDs> create_query_uuids TSA_GUARDED_BY(mutex);
    std::unordered_set<String /* root_zk_path */> acquired_data_in_keeper_map_tables TSA_GUARDED_BY(mutex);
    std::unordered_set<String /* table_zk_path */> acquired_shared_databases;

    mutable std::mutex mutex;

    std::atomic<bool> is_finished = false;
    std::atomic<bool> is_error_set = false;
};

}
