#include <sys/stat.h>

#include <string>

#include "caffe/datum_DB.hpp"

namespace caffe {

void DatumLMDB::Open() {
  if (*is_opened_ == false) {
    LOG(INFO) << "Opening lmdb " << param_.source();
    mdb_status_ = mdb_env_create(&mdb_env_);
    CHECK_EQ(mdb_status_, MDB_SUCCESS) << mdb_strerror(mdb_status_);
    mdb_cursor_ = NULL;
    switch (param_.mode()) {
    case DatumDBParameter_Mode_NEW:
      CHECK_EQ(mkdir(param_.source().c_str(), 0744), 0)
        << "mkdir " << param_.source() << "failed";
    case DatumDBParameter_Mode_WRITE:
      mdb_status_ = mdb_env_set_mapsize(mdb_env_, param_.mdb_env_mapsize());
      CHECK_EQ(mdb_status_, MDB_SUCCESS) << mdb_strerror(mdb_status_);
      mdb_status_ = mdb_env_open(mdb_env_, param_.source().c_str(), 0, 0664);
      CHECK_EQ(mdb_status_, MDB_SUCCESS) << mdb_strerror(mdb_status_);
      mdb_status_ = mdb_txn_begin(mdb_env_, NULL, 0, &mdb_txn_);
      CHECK_EQ(mdb_status_, MDB_SUCCESS) << mdb_strerror(mdb_status_);
      mdb_status_ = mdb_dbi_open(mdb_txn_, NULL, MDB_CREATE, &mdb_dbi_);
      CHECK_EQ(mdb_status_, MDB_SUCCESS) << mdb_strerror(mdb_status_);
      break;
    case DatumDBParameter_Mode_READ:
      mdb_status_ = mdb_env_set_mapsize(mdb_env_, 1);
      CHECK_EQ(mdb_status_, MDB_SUCCESS) << mdb_strerror(mdb_status_);
      mdb_status_ = mdb_env_set_maxreaders(mdb_env_, param_.mdb_env_maxreaders());
      CHECK_EQ(mdb_status_, MDB_SUCCESS) << mdb_strerror(mdb_status_);
      mdb_status_ = mdb_env_open(mdb_env_, param_.source().c_str(),
                    MDB_RDONLY|MDB_NOTLS|MDB_NOLOCK, 0664);
      CHECK_EQ(mdb_status_, MDB_SUCCESS) << mdb_strerror(mdb_status_);
      mdb_status_ = mdb_txn_begin(mdb_env_, NULL, MDB_RDONLY, &mdb_txn_);
      CHECK_EQ(mdb_status_, MDB_SUCCESS) << mdb_strerror(mdb_status_);
      mdb_status_ = mdb_dbi_open(mdb_txn_, NULL, 0, &mdb_dbi_);
      CHECK_EQ(mdb_status_, MDB_SUCCESS) << mdb_strerror(mdb_status_);
      break;
    default:
      LOG(FATAL) << "Unknown DB Mode " << param_.mode();
    }
    *is_opened_ = true;
  }
}

void DatumLMDB::Close() {
  if (*is_opened_) {
    if (mdb_cursor_ != NULL) {
      LOG(INFO) << "Closing Generator on " << param_.source();
      mdb_cursor_close(mdb_cursor_);
    }
    if (is_opened_.unique()) {
      LOG(INFO) << "Closing lmdb " << param_.source();
      mdb_close(mdb_env_, mdb_dbi_);
      mdb_txn_abort(mdb_txn_);
      mdb_env_close(mdb_env_);
    }
  }
}

shared_ptr<DatumDB::Generator> DatumLMDB::NewGenerator() {
  CHECK_EQ(param_.mode(),DatumDBParameter_Mode_READ)
    << "Only DatumDB in Mode_READ can use NewGenerator";
  CHECK(*is_opened_);
  LOG(INFO) << "Creating Generator for " << param_.source();
  DatumLMDB* datumdb = new DatumLMDB(*this);
  MDB_cursor* mdb_cursor;
  int mdb_status = mdb_cursor_open(mdb_txn_, mdb_dbi_, &mdb_cursor);
  CHECK_EQ(mdb_status, MDB_SUCCESS) << mdb_strerror(mdb_status);
  datumdb->mdb_cursor_ = mdb_cursor;
  shared_ptr<DatumDB::Generator> generator;
  generator.reset(new DatumDB::Generator(shared_ptr<DatumDB>(datumdb)));
  return generator;
}

bool DatumLMDB::Get(const string& key, Datum* datum) {
  string aux_key(key);
  MDB_val mdb_key, mdb_value;
  mdb_key.mv_size = aux_key.size();
  mdb_key.mv_data = reinterpret_cast<void*>(&aux_key[0]);
  int mdb_status = mdb_get(mdb_txn_, mdb_dbi_, &mdb_key, &mdb_value);
  if (mdb_status == MDB_SUCCESS) {
    datum->ParseFromArray(mdb_value.mv_data, mdb_value.mv_size);
    return true;
  } else {
    LOG(ERROR) << mdb_strerror(mdb_status);
    return false;
  }
}

void DatumLMDB::Put(const string& key, const Datum& datum) {
  CHECK(param_.mode() != DatumDBParameter_Mode_READ);
  string aux_key(key);
  MDB_val mdb_key, mdb_value;
  mdb_key.mv_size = aux_key.size();
  mdb_key.mv_data = reinterpret_cast<void*>(&aux_key[0]);
  string value;
  datum.SerializeToString(&value);
  mdb_value.mv_size = value.size();
  mdb_value.mv_data = reinterpret_cast<void*>(&value[0]);
  int mdb_status = mdb_put(mdb_txn_, mdb_dbi_, &mdb_key, &mdb_value, 0);
  CHECK_EQ(mdb_status, MDB_SUCCESS) << mdb_strerror(mdb_status);
}

void DatumLMDB::Commit() {
  CHECK(param_.mode() != DatumDBParameter_Mode_READ);
  int mdb_status = mdb_txn_commit(mdb_txn_);
  CHECK_EQ(mdb_status, MDB_SUCCESS) << mdb_strerror(mdb_status);
  mdb_status = mdb_txn_begin(mdb_env_, NULL, 0, &mdb_txn_);
  CHECK_EQ(mdb_status, MDB_SUCCESS) << mdb_strerror(mdb_status);
}

bool DatumLMDB::Valid() {
  if (mdb_status_ == MDB_SUCCESS) {
    return (mdb_value_.mv_data && mdb_value_.mv_size > 0
            && mdb_key_.mv_data && mdb_key_.mv_size > 0);
  } else {
    return false;
  }
}

bool DatumLMDB::Reset() {
  CHECK(mdb_cursor_);
  mdb_status_ = mdb_cursor_get(mdb_cursor_, &mdb_key_, &mdb_value_, MDB_FIRST);
  if (mdb_status_ != MDB_SUCCESS) {
    LOG(ERROR) << mdb_strerror(mdb_status_);
  }
  return Valid();
}

bool DatumLMDB::Next() {
  CHECK(mdb_cursor_);
  mdb_status_ = mdb_cursor_get(mdb_cursor_, &mdb_key_, &mdb_value_, MDB_NEXT);
  if (mdb_status_ != MDB_SUCCESS) {
    if (param_.loop()) {
      DLOG(INFO) << "Reached the end and looping.";
      return Reset();
    } else {
      if (mdb_status_ == MDB_NOTFOUND) {
        LOG(ERROR) << "Reached the end and not looping.";
      } else {
        LOG(ERROR) << mdb_strerror(mdb_status_);
      }
    }
  }
  return Valid();
}

bool DatumLMDB::Current(string* key, Datum* datum) {
  if (Valid()) {
    key->assign(string(reinterpret_cast<char*>(mdb_key_.mv_data),
                      mdb_key_.mv_size));
    datum->ParseFromArray(mdb_value_.mv_data, mdb_value_.mv_size);
    return true;
  } else {
    return false;
  }
}

REGISTER_DATUMDB_CLASS("lmdb", DatumLMDB);
}  // namespace caffe
