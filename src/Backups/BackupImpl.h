#pragma once

#include "config.h"
#include <Backups/BackupFactory.h>
#include <Backups/IBackup.h>
#include <Backups/IBackupCoordination.h>
#include <Backups/BackupInfo.h>
#include <map>
#include <mutex>


namespace DB
{
class IBackupCoordination;
class IBackupReader;
class IBackupWriter;
class SeekableReadBuffer;
class IArchiveReader;
class IArchiveWriter;

/// Implementation of IBackup.
/// Along with passed files it also stores backup metadata - a single file named ".backup" in XML format
/// which contains a list of all files in the backup with their sizes and checksums and information
/// whether the base backup should be used for each entry.
class BackupImpl : public IBackup
{
public:
    struct ArchiveParams
    {
        String archive_name;
        String password;
        String compression_method;
        int compression_level = 0;
        size_t max_volume_size = 0;
    };

    using SnapshotReaderCreator = std::function<std::shared_ptr<IBackupReader>(const String &, const String &)>;

    /// RESTORE
    BackupImpl(
        BackupFactory::CreateParams params_,
        const ArchiveParams & archive_params_,
        std::shared_ptr<IBackupReader> reader_,
        SnapshotReaderCreator lightweight_snapshot_reader_creator_ = {});

    /// BACKUP
    BackupImpl(
        BackupFactory::CreateParams params_,
        const ArchiveParams & archive_params_,
        std::shared_ptr<IBackupWriter> writer_);

    /// UNLOCK
    BackupImpl(
        const BackupInfo & backup_info_,
        const ArchiveParams & archive_params_,
        std::shared_ptr<IBackupReader> reader_,
        std::shared_ptr<IBackupWriter> lightweight_snapshot_writer_);

    ~BackupImpl() override;

    const String & getNameForLogging() const override { return backup_name_for_logging; }
    OpenMode getOpenMode() const override { return open_mode; }
    time_t getTimestamp() const override { return timestamp; }
    UUID getUUID() const override { return *uuid; }
    BackupPtr getBaseBackup() const override;
    size_t getNumFiles() const override;
    UInt64 getTotalSize() const override;
    size_t getNumEntries() const override;
    UInt64 getSizeOfEntries() const override;
    UInt64 getUncompressedSize() const override;
    UInt64 getCompressedSize() const override;
    size_t getNumReadFiles() const override;
    UInt64 getNumReadBytes() const override;
    bool directoryExists(const String & directory) const override;
    Strings listFiles(const String & directory, bool recursive) const override;
    bool hasFiles(const String & directory) const override;
    bool fileExists(const String & file_name) const override;
    bool fileExists(const SizeAndChecksum & size_and_checksum) const override;
    UInt64 getFileSize(const String & file_name) const override;
    UInt128 getFileChecksum(const String & file_name) const override;
    SizeAndChecksum getFileSizeAndChecksum(const String & file_name) const override;
    std::unique_ptr<ReadBufferFromFileBase> readFile(const String & file_name) const override;
    std::unique_ptr<ReadBufferFromFileBase> readFile(const String & file_name, const SizeAndChecksum & size_and_checksum) const override;
    size_t copyFileToDisk(const String & file_name, DiskPtr destination_disk, const String & destination_path, WriteMode write_mode) const override;
    size_t copyFileToDisk(const SizeAndChecksum & size_and_checksum, DiskPtr destination_disk, const String & destination_path, WriteMode write_mode) const override;
    void writeFile(const BackupFileInfo & info, BackupEntryPtr entry) override;
    bool supportsWritingInMultipleThreads() const override { return !use_archive; }
    void finalizeWriting() override;
    bool setIsCorrupted() noexcept override;
    bool tryRemoveAllFiles() noexcept override;
    bool tryRemoveAllFilesUnderDirectory(const String & directory) const noexcept override;

private:
    void open();
    void close();

    void openArchive();
    void closeArchive(bool finalize);

    /// Writes the file ".backup" containing backup's metadata.
    void writeBackupMetadata() TSA_REQUIRES(mutex);
    void readBackupMetadata() TSA_REQUIRES(mutex);

#if CLICKHOUSE_CLOUD
    size_t copyFileToDiskByObjectKey(const String & object_key, DiskPtr destination_disk, const String & destination_path, WriteMode write_mode) const;
#endif

    String getObjectKey(const String & file_name) const;
    std::unique_ptr<ReadBufferFromFileBase> readFileByObjectKey(const BackupFileInfo & info) const;

    /// Returns the base backup or null if there is no base backup.
    std::shared_ptr<const IBackup> getBaseBackupUnlocked() const TSA_REQUIRES(mutex);

    /// Checks that a new backup doesn't exist yet.
    void checkBackupDoesntExist() const;

    /// Lock file named ".lock" and containing the UUID of a backup is used to own the place where we're writing the backup.
    /// Thus it will not be allowed to put any other backup to the same place (even if the BACKUP command is executed on a different node).
    void createLockFile();
    bool checkLockFile(bool throw_if_failed) const;
    void removeLockFile();

    /// Calculates and sets `compressed_size`.
    void setCompressedSize();

    std::unique_ptr<ReadBufferFromFileBase>
    readFileImpl(const String & file_name, const SizeAndChecksum & size_and_checksum, bool read_encrypted) const;

    const BackupFactory::CreateParams params;
    BackupInfo backup_info;
    const String backup_name_for_logging;
    const bool use_archive;
    const ArchiveParams archive_params;
    const OpenMode open_mode;
    /// Used to write data to destinated object storage.
    std::shared_ptr<IBackupWriter> writer;
    /// Used to read data from backup files.
    std::shared_ptr<IBackupReader> reader;
    /// Only used for lightweight backup, we read data from original object storage so the endpoint may be different from the backup files.
    std::shared_ptr<IBackupReader> lightweight_snapshot_reader;
    std::shared_ptr<IBackupWriter> lightweight_snapshot_writer;
    SnapshotReaderCreator lightweight_snapshot_reader_creator;
    String original_endpoint; /// endpoint of source disk, we need to write it to metafile to restore a snapshot.
    String original_namespace; /// namespace of source disk, we need to write it to metafile to restore a snapshot.

    std::shared_ptr<IBackupCoordination> coordination;

    mutable std::mutex mutex;

    using SizeAndChecksum = std::pair<UInt64, UInt128>;
    std::map<String /* file_name */, SizeAndChecksum> file_names TSA_GUARDED_BY(mutex); /// Should be ordered alphabetically, see listFiles(). For empty files we assume checksum = 0.
    std::map<SizeAndChecksum, BackupFileInfo> file_infos TSA_GUARDED_BY(mutex); /// Information about files. Without empty files.
    /// object_key -> file name, only used by lightweight snapshot
    std::unordered_map<String, String> file_object_keys TSA_GUARDED_BY(mutex);
    std::unordered_map<String, BackupFileInfo> lightweight_snapshot_file_infos TSA_GUARDED_BY(mutex);

    std::optional<UUID> uuid;
    time_t timestamp = 0;
    size_t num_files = 0;
    UInt64 total_size = 0;
    size_t num_entries = 0;
    UInt64 size_of_entries = 0;
    UInt64 uncompressed_size = 0;
    UInt64 compressed_size = 0;
    mutable size_t num_read_files = 0;
    mutable UInt64 num_read_bytes = 0;
    int version;
    mutable std::optional<BackupInfo> base_backup_info;
    mutable std::shared_ptr<const IBackup> base_backup;
    mutable std::optional<UUID> base_backup_uuid;
    std::shared_ptr<IArchiveReader> archive_reader;
    std::shared_ptr<IArchiveWriter> archive_writer;
    String lock_file_name;
    std::atomic<bool> lock_file_before_first_file_checked = false;

    bool writing_finalized = false;
    bool corrupted = false;
    const LoggerPtr log;
};

}
