#include <fstream>
#include <utility>
#include "glog/logging.h"
#include "gtest/gtest.h"
#include "tendisplus/utils/status.h"
#include "tendisplus/utils/scopeguard.h"
#include "tendisplus/utils/portable.h"
#include "tendisplus/storage/rocks/rocks_kvstore.h"
#include "tendisplus/server/server_params.h"

namespace tendisplus {

static std::shared_ptr<ServerParams> genParams() {
    const auto guard = MakeGuard([] {
        remove("a.cfg");
    });
    std::ofstream myfile;
    myfile.open("a.cfg");
    myfile << "bind 127.0.0.1\n";
    myfile << "port 8903\n";
    myfile << "loglevel debug\n";
    myfile << "logdir ./log\n";
    myfile << "storageEngine rocks\n";
    myfile << "dbPath ./db\n";
    myfile << "rocksBlockCacheMB 4096\n";
    myfile.close();
    auto cfg = std::make_shared<ServerParams>();
    auto s = cfg->parseFile("a.cfg");
    EXPECT_EQ(s.ok(), true) << s.toString();
    return cfg;
}

TEST(RocksKVStore, Cursor) {
    auto cfg = genParams();
    EXPECT_TRUE(filesystem::create_directory("db"));
    EXPECT_TRUE(filesystem::create_directory("log"));
    const auto guard = MakeGuard([] {
        filesystem::remove_all("./log");
        filesystem::remove_all("./db");
    });
    auto blockCache =
        rocksdb::NewLRUCache(cfg->rocksBlockcacheMB * 1024 * 1024LL, 4);
    auto kvstore = std::make_unique<RocksKVStore>(
        "0",
        cfg,
        blockCache);

    auto eTxn1 = kvstore->createTransaction();
    EXPECT_EQ(eTxn1.ok(), true);
    std::unique_ptr<Transaction> txn1 = std::move(eTxn1.value());

    Status s = kvstore->setKV(
        Record(
            RecordKey(RecordType::RT_KV, "a", ""),
            RecordValue("txn1")),
        txn1.get());
    EXPECT_EQ(s.ok(), true);

    s = kvstore->setKV(
        Record(
            RecordKey(RecordType::RT_KV, "ab", ""),
            RecordValue("txn1")),
        txn1.get());
    EXPECT_EQ(s.ok(), true);

    s = kvstore->setKV(
        Record(
            RecordKey(RecordType::RT_KV, "abc", ""),
            RecordValue("txn1")),
        txn1.get());
    EXPECT_EQ(s.ok(), true);

    s = kvstore->setKV(
        Record(
            RecordKey(RecordType::RT_KV, "b", ""),
            RecordValue("txn1")),
        txn1.get());
    EXPECT_EQ(s.ok(), true);

    s = kvstore->setKV(
        Record(
            RecordKey(RecordType::RT_KV, "bac", ""),
            RecordValue("txn1")),
        txn1.get());
    EXPECT_EQ(s.ok(), true);

    std::unique_ptr<Cursor> cursor = txn1->createCursor();
    int32_t cnt = 0;
    while (true) {
        auto v = cursor->next();
        if (!v.ok()) {
            EXPECT_EQ(cnt, 5);
            EXPECT_EQ(v.status().code(), ErrorCodes::ERR_EXHAUST);
            break;
        }
        cnt += 1;
    }
    cnt = 0;
    std::string prefix;
    prefix.push_back(0);
    prefix.push_back(rt2Char(RecordType::RT_KV));
    prefix.push_back('b');
    cursor->seek(prefix);
    while (true) {
        auto v = cursor->next();
        if (!v.ok()) {
            EXPECT_EQ(cnt, 2);
            EXPECT_EQ(v.status().code(), ErrorCodes::ERR_EXHAUST);
            break;
        }
        cnt += 1;
    }
}

TEST(RocksKVStore, Backup) {
    auto cfg = genParams();
    EXPECT_TRUE(filesystem::create_directory("db"));
    EXPECT_TRUE(filesystem::create_directory("log"));
    const auto guard = MakeGuard([] {
        filesystem::remove_all("./log");
        filesystem::remove_all("./db");
    });
    auto blockCache =
        rocksdb::NewLRUCache(cfg->rocksBlockcacheMB * 1024 * 1024LL, 4);
    auto kvstore = std::make_unique<RocksKVStore>(
        "0",
        cfg,
        blockCache);

    auto eTxn1 = kvstore->createTransaction();
    EXPECT_EQ(eTxn1.ok(), true);
    std::unique_ptr<Transaction> txn1 = std::move(eTxn1.value());
    Status s = kvstore->setKV(
        Record(
            RecordKey(RecordType::RT_KV, "a", ""),
            RecordValue("txn1")),
        txn1.get());
    EXPECT_EQ(s.ok(), true);
    Expected<uint64_t> exptCommitId = txn1->commit();
    EXPECT_EQ(exptCommitId.ok(), true);

    Expected<BackupInfo> expBk = kvstore->backup();
    EXPECT_TRUE(expBk.ok()) << expBk.status().toString();
    for (auto& bk : expBk.value().getFileList()) {
        LOG(INFO) << "backupInfo:[" << bk.first << "," << bk.second << "]";
    }
    Expected<BackupInfo> expBk1 = kvstore->backup();
    EXPECT_FALSE(expBk1.ok());

    s = kvstore->stop();
    EXPECT_TRUE(s.ok());

    s = kvstore->clear();
    EXPECT_TRUE(s.ok());

    s = kvstore->restart(true);
    EXPECT_TRUE(s.ok());

    LOG(INFO) << "here";
    eTxn1 = kvstore->createTransaction();
    EXPECT_EQ(eTxn1.ok(), true);
    txn1 = std::move(eTxn1.value());
    Expected<RecordValue> e = kvstore->getKV(
        RecordKey(RecordType::RT_KV, "a", ""),
        txn1.get());
    EXPECT_EQ(e.ok(), true);
    LOG(INFO) << "here1";
}

TEST(RocksKVStore, Stop) {
    auto cfg = genParams();
    EXPECT_TRUE(filesystem::create_directory("db"));
    EXPECT_TRUE(filesystem::create_directory("log"));
    const auto guard = MakeGuard([] {
        filesystem::remove_all("./log");
        filesystem::remove_all("./db");
    });
    auto blockCache =
        rocksdb::NewLRUCache(cfg->rocksBlockcacheMB * 1024 * 1024LL, 4);
    auto kvstore = std::make_unique<RocksKVStore>(
        "0",
        cfg,
        blockCache);
    auto eTxn1 = kvstore->createTransaction();
    EXPECT_EQ(eTxn1.ok(), true);

    auto s = kvstore->stop();
    EXPECT_FALSE(s.ok());

    s = kvstore->clear();
    EXPECT_FALSE(s.ok());

    s = kvstore->restart(false);
    EXPECT_FALSE(s.ok());

    eTxn1.value().reset();

    s = kvstore->stop();
    EXPECT_TRUE(s.ok());

    s = kvstore->clear();
    EXPECT_TRUE(s.ok());

    s = kvstore->restart(false);
    EXPECT_TRUE(s.ok());
}

TEST(RocksKVStore, Common) {
    auto cfg = genParams();
    EXPECT_TRUE(filesystem::create_directory("db"));
    // EXPECT_TRUE(filesystem::create_directory("db/0"));
    EXPECT_TRUE(filesystem::create_directory("log"));
    const auto guard = MakeGuard([] {
        filesystem::remove_all("./log");
        filesystem::remove_all("./db");
    });
    auto blockCache =
        rocksdb::NewLRUCache(cfg->rocksBlockcacheMB * 1024 * 1024LL, 4);
    auto kvstore = std::make_unique<RocksKVStore>(
        "0",
        cfg,
        blockCache);
    auto eTxn1 = kvstore->createTransaction();
    auto eTxn2 = kvstore->createTransaction();
    EXPECT_EQ(eTxn1.ok(), true);
    EXPECT_EQ(eTxn2.ok(), true);
    std::unique_ptr<Transaction> txn1 = std::move(eTxn1.value());
    std::unique_ptr<Transaction> txn2 = std::move(eTxn2.value());

    std::set<uint64_t> uncommitted = kvstore->getUncommittedTxns();
    EXPECT_NE(uncommitted.find(
        dynamic_cast<RocksOptTxn*>(txn1.get())->getTxnId()),
        uncommitted.end());
    EXPECT_NE(uncommitted.find(
        dynamic_cast<RocksOptTxn*>(txn2.get())->getTxnId()),
        uncommitted.end());

    Status s = kvstore->setKV(
        Record(
            RecordKey(RecordType::RT_KV, "a", ""),
            RecordValue("txn1")),
        txn1.get());
    EXPECT_EQ(s.ok(), true);
    Expected<RecordValue> e = kvstore->getKV(
        RecordKey(RecordType::RT_KV, "a", ""),
        txn1.get());
    EXPECT_EQ(e.ok(), true);
    EXPECT_EQ(e.value(), RecordValue("txn1"));

    Expected<RecordValue> e1 = kvstore->getKV(
        RecordKey(RecordType::RT_KV, "a", ""),
        txn2.get());
    EXPECT_EQ(e1.status().code(), ErrorCodes::ERR_NOTFOUND);
    s = kvstore->setKV(
        Record(
            RecordKey(RecordType::RT_KV, "a", ""),
            RecordValue("txn2")),
        txn2.get());

    Expected<uint64_t> exptCommitId = txn2->commit();
    EXPECT_EQ(exptCommitId.ok(), true);
    exptCommitId = txn1->commit();
    EXPECT_EQ(exptCommitId.status().code(), ErrorCodes::ERR_COMMIT_RETRY);
    uncommitted = kvstore->getUncommittedTxns();
    EXPECT_EQ(uncommitted.find(
        dynamic_cast<RocksOptTxn*>(txn1.get())->getTxnId()),
        uncommitted.end());
    EXPECT_EQ(uncommitted.find(
        dynamic_cast<RocksOptTxn*>(txn2.get())->getTxnId()),
        uncommitted.end());
}

}  // namespace tendisplus
