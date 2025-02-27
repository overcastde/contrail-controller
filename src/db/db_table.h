/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#ifndef ctrlplane_db_table_h
#define ctrlplane_db_table_h

#include <memory>
#include <vector>
#include <boost/function.hpp>
#include <tbb/atomic.h>

#include "base/util.h"

class DB;
class DBClient;
class DBEntryBase;
class DBEntry;
class DBTablePartBase;
class DBTablePartition;
class ShowTableListener;

class DBRequestKey {
public:
    virtual ~DBRequestKey() { }
};
class DBRequestData {
public:
    virtual ~DBRequestData() { }
};

struct DBRequest {
    typedef enum {
        DB_ENTRY_ADD_CHANGE = 1,
        DB_ENTRY_DELETE = 2,
    } DBOperation;
    DBOperation oper;

    DBRequest();
    DBRequest(DBOperation op) : oper(op) { }
    ~DBRequest();

    std::auto_ptr<DBRequestKey> key;
    std::auto_ptr<DBRequestData> data;

    // Swap contents between two DBRequest entries.
    void Swap(DBRequest *rhs);

private:
    DISALLOW_COPY_AND_ASSIGN(DBRequest);
};

// Database table interface.
class DBTableBase {
public:
    typedef boost::function<void(DBTablePartBase *, DBEntryBase *)> ChangeCallback;
    typedef int ListenerId;

    static const int kInvalidId = -1;

    DBTableBase(DB *db, const std::string &name);
    virtual ~DBTableBase();

    // Enqueue a request to the table. Takes ownership of the data.
    bool Enqueue(DBRequest *req);
    void EnqueueRemove(DBEntryBase *db_entry);

    // Determine the table partition depending on the record key.
    virtual DBTablePartBase *GetTablePartition(const DBRequestKey *key) = 0;
    // Determine the table partition depending on the Entry 
    virtual DBTablePartBase *GetTablePartition(const DBEntryBase *entry) = 0;
    // Determine the table partition for given index
    virtual DBTablePartBase *GetTablePartition(const int index) = 0;

    // Record has been modified.
    virtual void Change(DBEntryBase *) = 0;


    // Register a DB listener.
    ListenerId Register(ChangeCallback callback,
        const std::string &name = "unspecified");
    void Unregister(ListenerId listener);

    void RunNotify(DBTablePartBase *tpart, DBEntryBase *entry);

    // Manage db state count for a listener.
    void AddToDBStateCount(ListenerId listener, int count);

    // Calculate the size across all partitions.
    // Must be called from Task which is mutually exclusive with db::DBTable.
    virtual size_t Size() const { return 0; }
    bool empty() const { return (Size() == 0); }

    // Suspended deletion resume hook for user function
    virtual void RetryDelete() { }
    virtual bool MayDelete() const;

    DB *database() { return db_; }
    const DB *database() const { return db_; }

    const std::string &name() const { return name_; }

    bool HasListeners() const;
    size_t GetListenerCount() const;
    void FillListeners(std::vector<ShowTableListener> *listeners) const;

    uint64_t enqueue_count() const { return enqueue_count_; }
    void incr_enqueue_count() { enqueue_count_++; }
    void reset_enqueue_count() { enqueue_count_ = 0; }

    uint64_t input_count() const { return input_count_; }
    void incr_input_count() { input_count_++; }
    void reset_input_count() { input_count_ = 0; }

    uint64_t notify_count() const { return notify_count_; }
    void incr_notify_count() { notify_count_++; }
    void reset_notify_count() { notify_count_ = 0; }

    bool HasWalkers() const { return walker_count_ != 0; }
    void incr_walker_count() { walker_count_++; }
    uint64_t decr_walker_count() { return --walker_count_; }

    uint64_t walk_request_count() const { return walk_request_count_; }
    uint64_t walk_complete_count() const { return walk_complete_count_; }
    uint64_t walk_cancel_count() const { return walk_cancel_count_; }
    void incr_walk_request_count() { walk_request_count_++; }
    void incr_walk_complete_count() { walk_complete_count_++; }
    void incr_walk_cancel_count() { walk_cancel_count_++; }

private:
    class ListenerInfo;
    DB *db_;
    std::string name_;
    std::auto_ptr<ListenerInfo> info_;
    uint64_t enqueue_count_;
    uint64_t input_count_;
    uint64_t notify_count_;
    uint64_t walker_count_;
    tbb::atomic<uint64_t> walk_request_count_;
    tbb::atomic<uint64_t> walk_complete_count_;
    tbb::atomic<uint64_t> walk_cancel_count_;
};

// An implementation of DBTableBase that uses boost::set as data-store
// Most of the DB Table implementations should derive from here instead of
// DBTableBase directly.
// Derive directly from DBTableBase only if there is a strong reason to do so
//
// Additionally, provides a set of virtual functions to override the default 
// functionality
class DBTable : public DBTableBase {
public:
    static bool WalkCallback(DBTablePartBase *tpart, DBEntryBase *entry);

    DBTable(DB *db, const std::string &name);
    virtual ~DBTable();
    void Init();

    ///////////////////////////////////////////////////////////
    // virtual functions to be implemented by derived class
    ///////////////////////////////////////////////////////////

    // Alloc a derived DBEntry
    virtual std::auto_ptr<DBEntry> AllocEntry(const DBRequestKey *key) const = 0;

    // Hash for an entry. Used to identify partition
    virtual size_t Hash(const DBEntry *entry) const {return 0;};

    // Hash for key. Used to identify partition
    virtual size_t Hash(const DBRequestKey *key) const {return 0;};

    // Alloc a derived DBTablePartBase entry. The default implementation
    // allocates DBTablePart should be good for most common cases.
    // Override if *really* necessary
    virtual DBTablePartition *AllocPartition(int index);

    // Input processing implemented by derived class. Default 
    // implementation takes care of Add/Delete/Change.
    // Override if *really* necessary
    virtual void Input(DBTablePartition *tbl_partition, DBClient *client,
                       DBRequest *req);

    // Add hook for user function. Must return entry to be inserted into tree
    // Must return NULL if no entry is to be added into tree
    virtual DBEntry *Add(const DBRequest *req);
    // Change hook for user function. Return 'true' if clients need to be
    // notified of the change
    virtual bool OnChange(DBEntry *entry, const DBRequest *req);
    // Delete hook for user function
    virtual bool Delete(DBEntry *entry, const DBRequest *req);

    void WalkCompleteCallback(DBTableBase *tbl_base);
    void NotifyAllEntries();

    ///////////////////////////////////////////////////////////
    // Utility methods for table
    ///////////////////////////////////////////////////////////
    // Find DB Entry. Get key from from argument
    DBEntry *Find(const DBEntry *entry);

    DBEntry *Find(const DBRequestKey *key);

    ///////////////////////////////////////////////////////////////
    // Virtual functions from DBTableBase implemented by DBTable
    ///////////////////////////////////////////////////////////////

    // Return the table partition for a specific request.
    virtual DBTablePartBase *GetTablePartition(const DBRequestKey *key);
    // Return the table partition for a DBEntryBase
    virtual DBTablePartBase *GetTablePartition(const DBEntryBase *entry);
    // Return the table partition for a index
    virtual DBTablePartBase *GetTablePartition(const int index);

    // Change notification handler.
    virtual void Change(DBEntryBase *entry);

    virtual int PartitionCount() const;

    // Calculate the size across all partitions.
    virtual size_t Size() const;

    // helper functions

    // Delete all the state entries of a specific listener.
    // Not thread-safe. Used to shutdown and cleanup the process.
    static void DBStateClear(DBTable *table, ListenerId id);

private:
    ///////////////////////////////////////////////////////////
    // Utility methods
    ///////////////////////////////////////////////////////////
    // Hash key to a partition id
    int GetPartitionId(const DBRequestKey *key);
    // Hash entry to a partition id
    int GetPartitionId(const DBEntry *entry);

    std::vector<DBTablePartition *> partitions_;
    int walk_id_;

    DISALLOW_COPY_AND_ASSIGN(DBTable);
};

#endif
