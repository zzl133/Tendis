#include <string>
#include <utility>
#include <memory>
#include <algorithm>
#include <cctype>
#include <clocale>
#include <map>
#include "glog/logging.h"
#include "tendisplus/utils/sync_point.h"
#include "tendisplus/utils/string.h"
#include "tendisplus/utils/invariant.h"
#include "tendisplus/utils/time.h"
#include "tendisplus/commands/command.h"

namespace tendisplus {

Expected<bool> expireBeforeNow(Session *sess,
                        RecordType type,
                        const std::string& key) {
    return Command::delKeyChkExpire(sess, key, type);
}

// return true if exists
// return false if not exists
// return error if has error
Expected<bool> expireAfterNow(Session *sess,
                        RecordType type,
                        const std::string& key,
                        uint64_t expireAt) {
    Expected<RecordValue> rv =
        Command::expireKeyIfNeeded(sess, key, type);
    if (rv.status().code() == ErrorCodes::ERR_EXPIRED) {
        return false;
    } else if (rv.status().code() == ErrorCodes::ERR_NOTFOUND) {
        return false;
    } else if (!rv.status().ok()) {
        return rv.status();
    }

    // record exists and not expired
    auto server = sess->getServerEntry();
    auto expdb = server->getSegmentMgr()->getDbWithKeyLock(sess, key, mgl::LockMode::LOCK_X);
    if (!expdb.ok()) {
        return expdb.status();
    }
    // uint32_t storeId = expdb.value().dbId;
    PStore kvstore = expdb.value().store;
    SessionCtx *pCtx = sess->getCtx();
    RecordKey rk(expdb.value().chunkId, pCtx->getDbId(), type, key, "");
    // if (Command::isKeyLocked(sess, storeId, rk.encode())) {
    //     return {ErrorCodes::ERR_BUSY, "key locked"};
    // }
    for (uint32_t i = 0; i < Command::RETRY_CNT; ++i) {
        auto ptxn = kvstore->createTransaction();
        if (!ptxn.ok()) {
            return ptxn.status();
        }
        std::unique_ptr<Transaction> txn = std::move(ptxn.value());
        Expected<RecordValue> eValue = kvstore->getKV(rk, txn.get());
        if (eValue.status().code() == ErrorCodes::ERR_NOTFOUND) {
            return false;
        } else if (!eValue.ok()) {
            return eValue.status();
        }
        auto rv = eValue.value();
        rv.setTtl(expireAt);
        Status s = kvstore->setKV(rk, rv, txn.get());
        if (!s.ok()) {
            return s;
        }

        auto commitStatus = txn->commit();
        s = commitStatus.status();
        if (s.ok()) {
            return true;
        } else if (s.code() != ErrorCodes::ERR_COMMIT_RETRY) {
            return s;
        }
        // status == ERR_COMMIT_RETRY
        if (i == Command::RETRY_CNT - 1) {
            return s;
        } else {
            continue;
        }
    }

    INVARIANT(0);
    return {ErrorCodes::ERR_INTERNAL, "not reachable"};
}

Expected<std::string> expireGeneric(Session *sess,
                                    uint64_t expireAt,
                                    const std::string& key) {
    if (expireAt >= nsSinceEpoch()/1000000) {
        bool atLeastOne = false;
        for (auto type : {RecordType::RT_KV,
                          RecordType::RT_LIST_META,
                          RecordType::RT_HASH_META,
                          RecordType::RT_SET_META,
                          RecordType::RT_ZSET_META}) {
            auto done = expireAfterNow(sess, type, key, expireAt);
            if (!done.ok()) {
                return done.status();
            }
            atLeastOne |= done.value();
        }
        return atLeastOne ? Command::fmtOne() : Command::fmtZero();
    } else {
        bool atLeastOne = false;
        for (auto type : {RecordType::RT_KV,
                          RecordType::RT_LIST_META,
                          RecordType::RT_HASH_META,
                          RecordType::RT_SET_META,
                          RecordType::RT_ZSET_META}) {
            auto done = expireBeforeNow(sess, type, key);
            if (!done.ok()) {
                return done.status();
            }
            atLeastOne |= done.value();
        }
        return atLeastOne ? Command::fmtOne() : Command::fmtZero();
    }
    INVARIANT(0);
    return {ErrorCodes::ERR_INTERNAL, "not reachable"};
}

class GeneralExpireCommand: public Command {
 public:
    GeneralExpireCommand(const std::string& name)
        :Command(name) {
    }

    ssize_t arity() const {
        return 3;
    }

    int32_t firstkey() const {
        return 1;
    }

    int32_t lastkey() const {
        return 1;
    }

    int32_t keystep() const {
        return 1;
    }

    Expected<std::string> run(Session *sess) final {
        const std::string& key = sess->getArgs()[1];
        auto expt = ::tendisplus::stoll(sess->getArgs()[2]);
        if (!expt.ok()) {
            return expt.status();
        }

        uint64_t millsecs = 0;
        if (Command::getName() == "expire") {
            millsecs = nsSinceEpoch()/1000000 + expt.value()*1000;
        } else if (Command::getName() == "pexpire") {
            millsecs = nsSinceEpoch()/1000000 + expt.value();
        } else if (Command::getName() == "expireat") {
            millsecs = expt.value()*1000;
        } else if (Command::getName() == "pexpireat") {
            millsecs = expt.value();
        } else {
            INVARIANT(0);
        }
        return expireGeneric(sess, millsecs, key);
    }
};

class ExpireCommand: public GeneralExpireCommand {
 public:
    ExpireCommand()
        :GeneralExpireCommand("expire") {
    }
} expireCmd;

class PExpireCommand: public GeneralExpireCommand {
 public:
    PExpireCommand()
        :GeneralExpireCommand("pexpire") {
    }
} pexpireCmd;

class ExpireAtCommand: public GeneralExpireCommand {
 public:
    ExpireAtCommand()
        :GeneralExpireCommand("expireat") {
    }
} expireatCmd;

class PExpireAtCommand: public GeneralExpireCommand {
 public:
    PExpireAtCommand()
        :GeneralExpireCommand("pexpireat") {
    }
} pexpireatCmd;

class GenericTtlCommand: public Command {
 public:
    GenericTtlCommand(const std::string& name)
        :Command(name) {
    }

    Expected<std::string> run(Session *sess) final {
        const std::string& key = sess->getArgs()[1];

        for (auto type : {RecordType::RT_KV,
                          RecordType::RT_LIST_META,
                          RecordType::RT_HASH_META,
                          RecordType::RT_SET_META,
                          RecordType::RT_ZSET_META}) {
            Expected<RecordValue> rv =
                Command::expireKeyIfNeeded(sess, key, type);
            if (rv.status().code() == ErrorCodes::ERR_EXPIRED) {
                continue;
            } else if (rv.status().code() == ErrorCodes::ERR_NOTFOUND) {
                continue;
            } else if (!rv.ok()) {
                return rv.status();
            }
            if (rv.value().getTtl() == 0) {
                return Command::fmtLongLong(-1);
            }
            int64_t ms = rv.value().getTtl() - nsSinceEpoch()/1000000;
            if (ms < 0) {
                ms = 1;
            }
            if (Command::getName() == "ttl") {
                return Command::fmtLongLong(ms/1000);
            } else if (Command::getName() == "pttl") {
                return Command::fmtLongLong(ms);
            } else {
                INVARIANT(0);
            }
        }
        return Command::fmtLongLong(-2);
    }
};

class TtlCommand: public GeneralExpireCommand {
 public:
    TtlCommand()
        :GeneralExpireCommand("ttl") {
    }
} ttlCmd;

class PTtlCommand: public GeneralExpireCommand {
 public:
    PTtlCommand()
        :GeneralExpireCommand("pttl") {
    }
} pttlCmd;

class ExistsCommand: public Command {
 public:
    ExistsCommand()
        :Command("exists") {
    }

    ssize_t arity() const {
        return 2;
    }

    int32_t firstkey() const {
        return 1;
    }

    int32_t lastkey() const {
        return 1;
    }

    int32_t keystep() const {
        return 1;
    }

    Expected<std::string> run(Session *sess) final {
        const std::string& key = sess->getArgs()[1];

        for (auto type : {RecordType::RT_KV,
                          RecordType::RT_LIST_META,
                          RecordType::RT_HASH_META,
                          RecordType::RT_SET_META,
                          RecordType::RT_ZSET_META}) {
            Expected<RecordValue> rv =
                Command::expireKeyIfNeeded(sess, key, type);
            if (rv.status().code() == ErrorCodes::ERR_EXPIRED) {
                continue;
            } else if (rv.status().code() == ErrorCodes::ERR_NOTFOUND) {
                continue;
            } else if (!rv.ok()) {
                return rv.status();
            }
            return Command::fmtOne();
        }
        return Command::fmtZero();
    }
} existsCmd;

class TypeCommand: public Command {
 public:
    TypeCommand()
        :Command("type") {
    }

    ssize_t arity() const {
        return 2;
    }

    int32_t firstkey() const {
        return 1;
    }

    int32_t lastkey() const {
        return 1;
    }

    int32_t keystep() const {
        return 1;
    }

    Expected<std::string> run(Session *sess) final {
        const std::string& key = sess->getArgs()[1];

        const std::map<RecordType, std::string> lookup = {
            {RecordType::RT_KV, "string"},
            {RecordType::RT_LIST_META, "list"},
            {RecordType::RT_HASH_META, "hash"},
            {RecordType::RT_SET_META, "set"},
            {RecordType::RT_ZSET_META, "zset"},
        };
        for (const auto& typestr : lookup) {
            Expected<RecordValue> rv =
                Command::expireKeyIfNeeded(sess, key, typestr.first);
            if (rv.status().code() == ErrorCodes::ERR_EXPIRED) {
                continue;
            } else if (rv.status().code() == ErrorCodes::ERR_NOTFOUND) {
                continue;
            } else if (!rv.ok()) {
                return rv.status();
            }
            return Command::fmtBulk(typestr.second);
        }
        return Command::fmtBulk("none");
    }
} typeCmd;
}  // namespace tendisplus