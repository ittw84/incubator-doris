// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "olap/delta_writer.h"

#include "olap/schema.h"
#include "olap/data_dir.h"
#include "olap/rowset/alpha_rowset_writer.h"
#include "olap/rowset/rowset_meta_manager.h"
#include "olap/rowset/rowset_id_generator.h"

namespace doris {

OLAPStatus DeltaWriter::open(WriteRequest* req, DeltaWriter** writer) {
    *writer = new DeltaWriter(req);
    return OLAP_SUCCESS;
}

DeltaWriter::DeltaWriter(WriteRequest* req)
    : _req(*req), _tablet(nullptr),
      _cur_rowset(nullptr), _new_rowset(nullptr), _new_tablet(nullptr),
      _rowset_writer(nullptr), _mem_table(nullptr),
      _schema(nullptr), _tablet_schema(nullptr),
      _delta_written_success(false) {}

DeltaWriter::~DeltaWriter() {
    if (!_delta_written_success) {
        _garbage_collection();
    }
    SAFE_DELETE(_mem_table);
    SAFE_DELETE(_schema);
}

void DeltaWriter::_garbage_collection() {
    // TODO(ygl): if cur_rowset is null, then the rowset is not built, should clean other
    // resources during load
    OLAPStatus rollback_status = TxnManager::instance()->rollback_txn(_req.partition_id,
        _req.txn_id,_req.tablet_id, _req.schema_hash);
    // has to check rollback status, because the rowset maybe committed in this thread and
    // published in another thread, then rollback will failed
    // when rollback failed should not delete rowset
    if (rollback_status == OLAP_SUCCESS) {
        StorageEngine::instance()->add_unused_rowset(_cur_rowset);
    }
    if (_new_tablet != nullptr) {
        rollback_status = TxnManager::instance()->rollback_txn(_req.partition_id, _req.txn_id,
            _new_tablet->tablet_id(), _new_tablet->schema_hash());
        if (rollback_status == OLAP_SUCCESS) {
            StorageEngine::instance()->add_unused_rowset(_new_rowset);
        }
    }
}

OLAPStatus DeltaWriter::init() {
    _tablet = TabletManager::instance()->get_tablet(_req.tablet_id, _req.schema_hash);
    if (_tablet == nullptr) {
        LOG(WARNING) << "tablet_id: " << _req.tablet_id << ", "
                     << "schema_hash: " << _req.schema_hash << " not found";
        return OLAP_ERR_TABLE_NOT_FOUND;
    }

    {
        MutexLock push_lock(_tablet->get_push_lock());
        RETURN_NOT_OK(TxnManager::instance()->prepare_txn(
                            _req.partition_id, _req.txn_id,
                            _req.tablet_id, _req.schema_hash, _req.load_id));
        if (_req.need_gen_rollup) {
            _tablet->obtain_header_rdlock();
            const AlterTabletTask& alter_task = _tablet->alter_task();
            AlterTabletState alter_state = alter_task.alter_state();
            TTabletId new_tablet_id = alter_task.related_tablet_id();
            TSchemaHash new_schema_hash = alter_task.related_schema_hash();;
            _tablet->release_header_lock();

            if (alter_state == ALTER_RUNNING) {
                LOG(INFO) << "load with schema change." << "old_tablet_id: " << _tablet->tablet_id() << ", "
                          << "old_schema_hash: " << _tablet->schema_hash() <<  ", "
                          << "new_tablet_id: " << new_tablet_id << ", "
                          << "new_schema_hash: " << new_schema_hash << ", "
                          << "transaction_id: " << _req.txn_id;
                _new_tablet = TabletManager::instance()->get_tablet(new_tablet_id, new_schema_hash);
                TxnManager::instance()->prepare_txn(
                                    _req.partition_id, _req.txn_id,
                                    new_tablet_id, new_schema_hash, _req.load_id);
            }
        }
    }

    RowsetId rowset_id = 0; // get rowset_id from id generator
    OLAPStatus status = _tablet->next_rowset_id(&rowset_id);
    if (status != OLAP_SUCCESS) {
        LOG(WARNING) << "generate rowset id failed, status:" << status;
        return OLAP_ERR_ROWSET_GENERATE_ID_FAILED;
    }
    RowsetWriterContextBuilder context_builder;
    context_builder.set_rowset_id(rowset_id)
            .set_tablet_id(_req.tablet_id)
            .set_partition_id(_req.partition_id)
            .set_tablet_schema_hash(_req.schema_hash)
            .set_rowset_type(ALPHA_ROWSET)
            .set_rowset_path_prefix(_tablet->tablet_path())
            .set_tablet_schema(&(_tablet->tablet_schema()))
            .set_rowset_state(PREPARED)
            .set_data_dir(_tablet->data_dir())
            .set_txn_id(_req.txn_id)
            .set_load_id(_req.load_id);
    RowsetWriterContext writer_context = context_builder.build();

    // TODO: new RowsetBuilder according to tablet storage type
    _rowset_writer.reset(new AlphaRowsetWriter());
    status = _rowset_writer->init(writer_context);
    if (status != OLAP_SUCCESS) {
        return OLAP_ERR_ROWSET_WRITER_INIT;
    }

    const std::vector<SlotDescriptor*>& slots = _req.tuple_desc->slots();
    const TabletSchema& schema = _tablet->tablet_schema();
    for (size_t col_id = 0; col_id < schema.num_columns(); ++col_id) {
        const TabletColumn& column = schema.column(col_id);
        for (size_t i = 0; i < slots.size(); ++i) {
            if (slots[i]->col_name() == column.name()) {
                _col_ids.push_back(i);
            }
        }
    }
    _tablet_schema = &(_tablet->tablet_schema());
    _schema = new Schema(*_tablet_schema);
    _mem_table = new MemTable(_schema, _tablet_schema, &_col_ids,
                              _req.tuple_desc, _tablet->keys_type());
    _is_init = true;
    return OLAP_SUCCESS;
}

OLAPStatus DeltaWriter::write(Tuple* tuple) {
    if (!_is_init) {
        auto st = init();
        if (st != OLAP_SUCCESS) {
            return st;
        }
    }

    _mem_table->insert(tuple);
    if (_mem_table->memory_usage() >= config::write_buffer_size) {
        RETURN_NOT_OK(_mem_table->flush(_rowset_writer));

        SAFE_DELETE(_mem_table);
        _mem_table = new MemTable(_schema, _tablet_schema, &_col_ids,
                                  _req.tuple_desc, _tablet->keys_type());
    }
    return OLAP_SUCCESS;
}

OLAPStatus DeltaWriter::close(google::protobuf::RepeatedPtrField<PTabletInfo>* tablet_vec) {
    if (!_is_init) {
        auto st = init();
        if (st != OLAP_SUCCESS) {
            return st;
        }
    }
    RETURN_NOT_OK(_mem_table->close(_rowset_writer));

    OLAPStatus res = OLAP_SUCCESS;
    // use rowset meta manager to save meta
    _cur_rowset = _rowset_writer->build();
    StorageEngine::instance()->remove_pending_paths(_rowset_writer->rowset_id());
    if (_cur_rowset == nullptr) {
        LOG(WARNING) << "fail to build rowset";
        return OLAP_ERR_MALLOC_ERROR;
    }
    res = TxnManager::instance()->commit_txn(_tablet->data_dir()->get_meta(),
        _req.partition_id, _req.txn_id,_req.tablet_id, _req.schema_hash,
        _req.load_id, _cur_rowset, false);
    if (res != OLAP_SUCCESS && res != OLAP_ERR_PUSH_TRANSACTION_ALREADY_EXIST) {
        LOG(WARNING) << "commit txn: " << _req.txn_id
                     << " for rowset: " << _cur_rowset->rowset_id()
                     << " failed.";
        return res;
    }

    if (_new_tablet != nullptr) {
        LOG(INFO) << "convert version for schema change";
        SchemaChangeHandler schema_change;
        res = schema_change.schema_version_convert(_tablet, _new_tablet, &_cur_rowset, &_new_rowset);
        if (res != OLAP_SUCCESS) {
            LOG(WARNING) << "failed to convert delta for new tablet in schema change."
                         << "res: " << res << ", "
                         << "new_tablet: " << _new_tablet->full_name();
            return res;
        }

        if (_new_rowset == nullptr) {
            LOG(WARNING) << "fail to build rowset";
            return OLAP_ERR_MALLOC_ERROR;
        }
        res = TxnManager::instance()->commit_txn(_new_tablet->data_dir()->get_meta(),
            _req.partition_id, _req.txn_id, _new_tablet->tablet_id(),
            _new_tablet->schema_hash(),
            _req.load_id, _new_rowset, false);

        if (res != OLAP_SUCCESS && res != OLAP_ERR_PUSH_TRANSACTION_ALREADY_EXIST) {
            LOG(WARNING) << "save pending rowset failed. rowset_id:"
                         << _new_rowset->rowset_id();
            return res;
        }
    }

#ifndef BE_TEST
    PTabletInfo* tablet_info = tablet_vec->Add();
    tablet_info->set_tablet_id(_tablet->tablet_id());
    tablet_info->set_schema_hash(_tablet->schema_hash());
    if (_new_tablet != nullptr) {
        tablet_info = tablet_vec->Add();
        tablet_info->set_tablet_id(_new_tablet->tablet_id());
        tablet_info->set_schema_hash(_new_tablet->schema_hash());
    }
#endif

    _delta_written_success = true;
    return OLAP_SUCCESS;
}

OLAPStatus DeltaWriter::cancel() {
    DCHECK(!_is_init);
    return OLAP_SUCCESS;
}

} // namespace doris
