/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2020 Couchbase, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#pragma once

#include <operations/document_id.hxx>
#include <operations/get.hxx>
#include <operations/upsert.hxx>
#include <operations/remove.hxx>
#include <operations/lookup_in.hxx>
#include <operations/mutate_in.hxx>

#include <operations/query.hxx>

#include <operations/bucket_get_all.hxx>
#include <operations/bucket_get.hxx>
#include <operations/bucket_drop.hxx>
#include <operations/bucket_flush.hxx>
#include <operations/bucket_create.hxx>
#include <operations/bucket_update.hxx>

#include <operations/scope_get_all.hxx>
#include <operations/scope_create.hxx>
#include <operations/scope_drop.hxx>
#include <operations/collection_create.hxx>
#include <operations/collection_drop.hxx>

#include <operations/cluster_developer_preview_enable.hxx>

#include <operations/query_index_get_all.hxx>
#include <operations/query_index_drop.hxx>
#include <operations/query_index_create.hxx>
#include <operations/query_index_build_deferred.hxx>

#include <operations/command.hxx>
