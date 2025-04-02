#include <Client/BuzzHouse/Generator/RandomSettings.h>
#include <Client/BuzzHouse/Generator/SQLCatalog.h>
#include <Client/BuzzHouse/Generator/SQLTypes.h>
#include <Client/BuzzHouse/Generator/StatementGenerator.h>
#include "SQLCatalog.h"

namespace BuzzHouse
{

StatementGenerator::StatementGenerator(FuzzConfig & fuzzc, ExternalIntegrations & conn, const bool scf, const bool rs)
    : fc(fuzzc)
    , connections(conn)
    , supports_cloud_features(scf)
    , replica_setup(rs)
    , deterministic_funcs_limit(static_cast<size_t>(
          std::find_if(CHFuncs.begin(), CHFuncs.end(), StatementGenerator::funcNotDeterministicIndexLambda) - CHFuncs.begin()))
    , deterministic_aggrs_limit(static_cast<size_t>(
          std::find_if(CHAggrs.begin(), CHAggrs.end(), StatementGenerator::aggrNotDeterministicIndexLambda) - CHAggrs.begin()))
{
    chassert(enum8_ids.size() > enum_values.size() && enum16_ids.size() > enum_values.size());

    for (size_t i = 0; i < deterministic_funcs_limit; i++)
    {
        /// Add single argument functions for non sargable predicates
        const CHFunction & next = CHFuncs[i];

        if (next.min_lambda_param == 0 && next.min_args == 1)
        {
            one_arg_funcs.push_back(next);
        }
    }
}

void StatementGenerator::generateStorage(RandomGenerator & rg, Storage * store) const
{
    store->set_storage(static_cast<Storage_DataStorage>((rg.nextRandomUInt32() % static_cast<uint32_t>(Storage::DataStorage_MAX)) + 1));
    store->set_storage_name(rg.pickRandomly(fc.disks));
}

void StatementGenerator::setRandomSetting(RandomGenerator & rg, const std::unordered_map<String, CHSetting> & settings, SetValue * set)
{
    const String & setting = rg.pickRandomly(settings);

    set->set_property(setting);
    set->set_value(settings.at(setting).random_func(rg));
}

void StatementGenerator::generateSettingValues(
    RandomGenerator & rg, const std::unordered_map<String, CHSetting> & settings, const size_t nvalues, SettingValues * vals)
{
    for (size_t i = 0; i < nvalues; i++)
    {
        setRandomSetting(rg, settings, vals->has_set_value() ? vals->add_other_values() : vals->mutable_set_value());
    }
}

void StatementGenerator::generateSettingValues(
    RandomGenerator & rg, const std::unordered_map<String, CHSetting> & settings, SettingValues * vals)
{
    generateSettingValues(rg, settings, std::min<size_t>(settings.size(), static_cast<size_t>((rg.nextRandomUInt32() % 20) + 1)), vals);
}

void StatementGenerator::generateSettingList(RandomGenerator & rg, const std::unordered_map<String, CHSetting> & settings, SettingList * sl)
{
    const size_t nvalues = std::min<size_t>(settings.size(), static_cast<size_t>((rg.nextRandomUInt32() % 7) + 1));

    for (size_t i = 0; i < nvalues; i++)
    {
        const String & next = rg.pickRandomly(settings);

        if (sl->has_setting())
        {
            sl->add_other_settings(next);
        }
        else
        {
            sl->set_setting(next);
        }
    }
}

DatabaseEngineValues StatementGenerator::getNextDatabaseEngine(RandomGenerator & rg)
{
    chassert(this->ids.empty());
    this->ids.emplace_back(DAtomic);
    this->ids.emplace_back(DMemory);
    if (replica_setup)
    {
        this->ids.emplace_back(DReplicated);
    }
    if (supports_cloud_features)
    {
        this->ids.emplace_back(DShared);
    }
    if (!fc.disks.empty())
    {
        this->ids.emplace_back(DBackup);
    }
    const auto res = static_cast<DatabaseEngineValues>(rg.pickRandomly(this->ids));
    this->ids.clear();
    return res;
}

void StatementGenerator::generateNextCreateDatabase(RandomGenerator & rg, CreateDatabase * cd)
{
    SQLDatabase next;
    const uint32_t dname = this->database_counter++;
    DatabaseEngine * deng = cd->mutable_dengine();

    next.deng = this->getNextDatabaseEngine(rg);
    deng->set_engine(next.deng);
    if (next.isReplicatedDatabase())
    {
        next.zoo_path_counter = this->zoo_path_counter++;
    }
    else if (next.isBackupDatabase())
    {
        next.backed_db = "d" + ((databases.empty() || rg.nextSmallNumber() < 3) ? "efault" : std::to_string(rg.pickRandomly(databases)));
        next.backed_disk = rg.pickRandomly(fc.disks);
    }
    if (!fc.clusters.empty() && rg.nextSmallNumber() < (next.isReplicatedOrSharedDatabase() ? 9 : 4))
    {
        next.cluster = rg.pickRandomly(fc.clusters);
        cd->mutable_cluster()->set_cluster(next.cluster.value());
    }
    next.dname = dname;
    next.finishDatabaseSpecification(deng);
    next.setName(cd->mutable_database());
    if (rg.nextSmallNumber() < 3)
    {
        cd->set_comment(rg.nextString("'", true, rg.nextRandomUInt32() % 1009));
    }
    this->staged_databases[dname] = std::make_shared<SQLDatabase>(std::move(next));
}

void StatementGenerator::generateNextCreateFunction(RandomGenerator & rg, CreateFunction * cf)
{
    SQLFunction next;
    const uint32_t fname = this->function_counter++;

    next.fname = fname;
    next.nargs = std::min(this->fc.max_width - this->width, (rg.nextMediumNumber() % fc.max_columns) + UINT32_C(1));
    if ((next.is_deterministic = rg.nextBool()))
    {
        /// If this function is later called by an oracle, then don't call it
        this->setAllowNotDetermistic(false);
        this->enforceFinal(true);
    }
    generateLambdaCall(rg, next.nargs, cf->mutable_lexpr());
    this->levels.clear();
    if (next.is_deterministic)
    {
        this->setAllowNotDetermistic(true);
        this->enforceFinal(false);
    }
    if (!fc.clusters.empty() && rg.nextSmallNumber() < 4)
    {
        next.cluster = rg.pickRandomly(fc.clusters);
        cf->mutable_cluster()->set_cluster(next.cluster.value());
    }
    next.setName(cf->mutable_function());
    this->staged_functions[fname] = std::move(next);
}

static void SetViewInterval(RandomGenerator & rg, RefreshInterval * ri)
{
    ri->set_interval(rg.nextSmallNumber() - 1);
    ri->set_unit(RefreshInterval_RefreshUnit::RefreshInterval_RefreshUnit_SECOND);
}

void StatementGenerator::generateNextRefreshableView(RandomGenerator & rg, RefreshableView * cv)
{
    const RefreshableView_RefreshPolicy pol = rg.nextBool() ? RefreshableView_RefreshPolicy::RefreshableView_RefreshPolicy_EVERY
                                                            : RefreshableView_RefreshPolicy::RefreshableView_RefreshPolicy_AFTER;

    cv->set_policy(pol);
    SetViewInterval(rg, cv->mutable_interval());
    if (pol == RefreshableView_RefreshPolicy::RefreshableView_RefreshPolicy_EVERY && rg.nextBool())
    {
        SetViewInterval(rg, cv->mutable_offset());
    }
    SetViewInterval(rg, cv->mutable_randomize());
    cv->set_append(rg.nextBool());
}

static void matchQueryAliases(const SQLView & v, Select * osel, Select * nsel)
{
    if (v.has_with_cols)
    {
        /// Make sure aliases match
        uint32_t i = 0;
        SelectStatementCore * ssc = nsel->mutable_select_core();

        for (const auto & entry : v.cols)
        {
            ExprColAlias * eca = ssc->add_result_columns()->mutable_eca();

            eca->mutable_expr()->mutable_comp_expr()->mutable_expr_stc()->mutable_col()->mutable_path()->mutable_col()->set_column(
                "c" + std::to_string(i));
            eca->mutable_col_alias()->set_column("c" + std::to_string(entry));
            i++;
        }
        ssc->mutable_from()
            ->mutable_tos()
            ->mutable_join_clause()
            ->mutable_tos()
            ->mutable_joined_table()
            ->mutable_tof()
            ->mutable_select()
            ->mutable_inner_query()
            ->mutable_select()
            ->set_allocated_sel(osel);
    }
    else
    {
        nsel->CopyFrom(*osel);
    }
}

void StatementGenerator::generateNextCreateView(RandomGenerator & rg, CreateView * cv)
{
    SQLView next;
    uint32_t tname = 0;
    const bool replace = collectionCount<SQLView>(attached_views) > 3 && rg.nextMediumNumber() < 16;
    const uint32_t view_ncols = (rg.nextMediumNumber() % fc.max_columns) + UINT32_C(1);

    if (replace)
    {
        const SQLView & v = rg.pickRandomly(filterCollection<SQLView>(attached_views));

        next.db = v.db;
        tname = next.tname = v.tname;
    }
    else
    {
        if (collectionHas<std::shared_ptr<SQLDatabase>>(attached_databases) && rg.nextSmallNumber() < 9)
        {
            next.db = rg.pickRandomly(filterCollection<std::shared_ptr<SQLDatabase>>(attached_databases));
        }
        tname = next.tname = this->table_counter++;
    }
    cv->set_create_opt(replace ? CreateReplaceOption::Replace : CreateReplaceOption::Create);
    next.is_materialized = rg.nextBool();
    cv->set_materialized(next.is_materialized);
    next.setName(cv->mutable_est(), false);
    if (next.is_materialized)
    {
        TableEngine * te = cv->mutable_engine();
        const uint32_t nopt = rg.nextSmallNumber();

        if (nopt < 4)
        {
            getNextTableEngine(rg, false, next);
            te->set_engine(next.teng);
        }
        else
        {
            next.is_deterministic = true;
            next.teng = TableEngineValues::MergeTree;
        }
        const auto & table_to_lambda = [&view_ncols, &next](const SQLTable & t)
        { return t.isAttached() && t.numberOfInsertableColumns() >= view_ncols && (t.is_deterministic || !next.is_deterministic); };
        next.has_with_cols = collectionHas<SQLTable>(table_to_lambda);
        const bool has_tables = next.has_with_cols || !tables.empty();
        const bool has_to
            = !replace && nopt > 6 && (next.has_with_cols || has_tables) && rg.nextSmallNumber() < (next.has_with_cols ? 9 : 6);

        chassert(this->entries.empty());
        for (uint32_t i = 0; i < view_ncols; i++)
        {
            std::vector<ColumnPathChainEntry> path = {ColumnPathChainEntry("c" + std::to_string(i), nullptr)};
            entries.emplace_back(ColumnPathChain(std::nullopt, ColumnSpecial::NONE, std::nullopt, std::move(path)));
        }
        if (!has_to)
        {
            SQLRelation rel("v" + std::to_string(next.tname));

            for (uint32_t i = 0; i < view_ncols; i++)
            {
                rel.cols.emplace_back(SQLRelationCol(rel.name, {"c" + std::to_string(i)}));
            }
            this->levels[this->current_level].rels.emplace_back(rel);
            this->levels[this->current_level].allow_aggregates = this->levels[this->current_level].allow_window_funcs = false;
            generateEngineDetails(rg, next, true, te);
            this->levels.clear();
        }
        if (next.isMergeTreeFamily() && rg.nextMediumNumber() < 16)
        {
            generateNextTTL(rg, std::nullopt, te, te->mutable_ttl_expr());
        }
        this->entries.clear();

        if (has_to)
        {
            CreateMatViewTo * cmvt = cv->mutable_to();
            SQLTable & t = const_cast<SQLTable &>(
                next.has_with_cols ? rg.pickRandomly(filterCollection<SQLTable>(table_to_lambda)).get()
                                   : rg.pickValueRandomlyFromMap(this->tables));

            t.setName(cmvt->mutable_est(), false);
            if (next.has_with_cols)
            {
                for (const auto & col : t.cols)
                {
                    if (col.second.canBeInserted())
                    {
                        filtered_columns.emplace_back(std::ref<const SQLColumn>(col.second));
                    }
                }
                if (rg.nextBool())
                {
                    std::shuffle(filtered_columns.begin(), filtered_columns.end(), rg.generator);
                }
                for (uint32_t i = 0; i < view_ncols; i++)
                {
                    SQLColumn col = filtered_columns[i].get();

                    addTableColumnInternal(rg, t, col.cname, false, false, ColumnSpecial::NONE, fc.type_mask, col, cmvt->add_col_list());
                    next.cols.insert(col.cname);
                }
                filtered_columns.clear();
            }
        }
        if (!replace && (next.is_refreshable = rg.nextBool()))
        {
            generateNextRefreshableView(rg, cv->mutable_refresh());
            cv->set_empty(rg.nextBool());
        }
        else
        {
            cv->set_populate(!has_to && !replace && rg.nextSmallNumber() < 4);
        }
    }
    else
    {
        next.is_deterministic = rg.nextSmallNumber() < 9;
    }
    if (next.cols.empty())
    {
        for (uint32_t i = 0; i < view_ncols; i++)
        {
            next.cols.insert(i);
        }
    }
    setClusterInfo(rg, next);
    if (next.cluster.has_value())
    {
        cv->mutable_cluster()->set_cluster(next.cluster.value());
    }
    if (next.is_deterministic)
    {
        this->setAllowNotDetermistic(false);
        this->enforceFinal(true);
    }
    this->levels[this->current_level] = QueryLevel(this->current_level);
    this->allow_in_expression_alias = rg.nextSmallNumber() < 3;
    generateSelect(
        rg,
        false,
        false,
        view_ncols,
        next.is_materialized ? (~allow_prewhere) : std::numeric_limits<uint32_t>::max(),
        cv->mutable_select());
    this->levels.clear();
    this->allow_in_expression_alias = true;
    if (next.is_deterministic)
    {
        this->setAllowNotDetermistic(true);
        this->enforceFinal(false);
    }
    matchQueryAliases(next, cv->release_select(), cv->mutable_select());
    if (rg.nextSmallNumber() < 3)
    {
        cv->set_comment(rg.nextString("'", true, rg.nextRandomUInt32() % 1009));
    }
    this->staged_views[tname] = next;
}

void StatementGenerator::generateNextDrop(RandomGenerator & rg, Drop * dp)
{
    SQLObjectName * sot = dp->mutable_object();
    const uint32_t drop_table = 10 * static_cast<uint32_t>(collectionCount<SQLTable>(attached_tables) > 3);
    const uint32_t drop_view = 10 * static_cast<uint32_t>(collectionCount<SQLView>(attached_views) > 3);
    const uint32_t drop_dictionary = 10 * static_cast<uint32_t>(collectionCount<SQLDictionary>(attached_dictionaries) > 3);
    const uint32_t drop_database = 2 * static_cast<uint32_t>(collectionCount<std::shared_ptr<SQLDatabase>>(attached_databases) > 3);
    const uint32_t drop_function = 1 * static_cast<uint32_t>(functions.size() > 3);
    const uint32_t prob_space = drop_table + drop_view + drop_dictionary + drop_database + drop_function;
    std::uniform_int_distribution<uint32_t> next_dist(1, prob_space);
    const uint32_t nopt = next_dist(rg.generator);
    std::optional<String> cluster;

    if (drop_table && nopt < (drop_table + 1))
    {
        const SQLTable & t = rg.pickRandomly(filterCollection<SQLTable>(attached_tables));

        cluster = t.getCluster();
        dp->set_is_temp(t.is_temp);
        dp->set_sobject(SQLObject::TABLE);
        dp->set_if_empty(rg.nextSmallNumber() < 4);
        t.setName(sot->mutable_est(), false);
    }
    else if (drop_view && nopt < (drop_table + drop_view + 1))
    {
        const SQLView & v = rg.pickRandomly(filterCollection<SQLView>(attached_views));

        cluster = v.getCluster();
        dp->set_sobject(SQLObject::VIEW);
        v.setName(sot->mutable_est(), false);
    }
    else if (drop_dictionary && nopt < (drop_table + drop_view + drop_dictionary + 1))
    {
        const SQLDictionary & d = rg.pickRandomly(filterCollection<SQLDictionary>(attached_dictionaries));

        cluster = d.getCluster();
        dp->set_sobject(SQLObject::DICTIONARY);
        d.setName(sot->mutable_est(), false);
    }
    else if (drop_database && nopt < (drop_table + drop_view + drop_dictionary + drop_database + 1))
    {
        const std::shared_ptr<SQLDatabase> & d = rg.pickRandomly(filterCollection<std::shared_ptr<SQLDatabase>>(attached_databases));

        cluster = d->getCluster();
        dp->set_sobject(SQLObject::DATABASE);
        d->setName(sot->mutable_database());
    }
    else if (drop_function && nopt < (drop_table + drop_view + drop_dictionary + drop_database + drop_function + 1))
    {
        const SQLFunction & f = rg.pickValueRandomlyFromMap(this->functions);

        cluster = f.getCluster();
        dp->set_sobject(SQLObject::FUNCTION);
        f.setName(sot->mutable_function());
    }
    else
    {
        chassert(0);
    }
    if (cluster.has_value())
    {
        dp->mutable_cluster()->set_cluster(cluster.value());
    }
    if (dp->sobject() != SQLObject::FUNCTION)
    {
        dp->set_sync(rg.nextSmallNumber() < 3);
        if (rg.nextSmallNumber() < 3)
        {
            generateSettingValues(rg, serverSettings, dp->mutable_setting_values());
        }
    }
}

void StatementGenerator::generateNextTablePartition(RandomGenerator & rg, const bool allow_parts, const SQLTable & t, PartitionExpr * pexpr)
{
    bool set_part = false;

    if (t.isMergeTreeFamily())
    {
        const String dname = t.db ? ("d" + std::to_string(t.db->dname)) : "";
        const String tname = "t" + std::to_string(t.tname);
        const bool table_has_partitions = rg.nextSmallNumber() < 9 && fc.tableHasPartitions(false, dname, tname);

        if (table_has_partitions)
        {
            if (allow_parts && rg.nextBool())
            {
                pexpr->set_part(fc.tableGetRandomPartitionOrPart(false, false, dname, tname));
            }
            else
            {
                pexpr->set_partition_id(fc.tableGetRandomPartitionOrPart(false, true, dname, tname));
            }
            set_part = true;
        }
    }
    if (!set_part)
    {
        pexpr->set_tuple(true);
    }
}

static const auto optimize_table_lambda = [](const SQLTable & t) { return t.isAttached() && t.isMergeTreeFamily(); };

void StatementGenerator::generateNextOptimizeTable(RandomGenerator & rg, OptimizeTable * ot)
{
    const SQLTable & t = rg.pickRandomly(filterCollection<SQLTable>(optimize_table_lambda));
    const std::optional<String> cluster = t.getCluster();

    t.setName(ot->mutable_est(), false);
    if (t.isMergeTreeFamily())
    {
        if (rg.nextBool())
        {
            generateNextTablePartition(rg, false, t, ot->mutable_single_partition()->mutable_partition());
        }
        ot->set_cleanup(rg.nextSmallNumber() < 3);
    }
    if (rg.nextSmallNumber() < 4)
    {
        const uint32_t noption = rg.nextMediumNumber();
        DeduplicateExpr * dde = ot->mutable_dedup();

        if (noption < 51)
        {
            ColumnPathList * clist = noption < 26 ? dde->mutable_col_list() : dde->mutable_ded_star_except();
            flatTableColumnPath(flat_tuple | flat_nested | skip_nested_node, t.cols, [](const SQLColumn &) { return true; });
            const uint32_t ocols
                = (rg.nextMediumNumber() % std::min<uint32_t>(static_cast<uint32_t>(this->entries.size()), UINT32_C(4))) + 1;
            std::shuffle(entries.begin(), entries.end(), rg.generator);
            for (uint32_t i = 0; i < ocols; i++)
            {
                columnPathRef(entries[i], i == 0 ? clist->mutable_col() : clist->add_other_cols());
            }
            entries.clear();
        }
        else if (noption < 76)
        {
            dde->set_ded_star(true);
        }
    }
    if (cluster.has_value())
    {
        ot->mutable_cluster()->set_cluster(cluster.value());
    }
    ot->set_final((t.supportsFinal() || t.isMergeTreeFamily()) && rg.nextSmallNumber() < 3);
    if (rg.nextSmallNumber() < 3)
    {
        generateSettingValues(rg, serverSettings, ot->mutable_setting_values());
    }
}

void StatementGenerator::generateNextCheckTable(RandomGenerator & rg, CheckTable * ct)
{
    const SQLTable & t = rg.pickRandomly(filterCollection<SQLTable>(attached_tables));

    t.setName(ct->mutable_est(), false);
    if (t.isMergeTreeFamily() && rg.nextBool())
    {
        generateNextTablePartition(rg, true, t, ct->mutable_single_partition()->mutable_partition());
    }
    if (rg.nextSmallNumber() < 3)
    {
        SettingValues * vals = ct->mutable_setting_values();

        generateSettingValues(rg, serverSettings, vals);
        if (rg.nextSmallNumber() < 3)
        {
            SetValue * sv = vals->add_other_values();

            sv->set_property("check_query_single_value_result");
            sv->set_value(rg.nextBool() ? "1" : "0");
        }
    }
    ct->set_single_result(rg.nextSmallNumber() < 4);
}

void StatementGenerator::generateNextDescTable(RandomGenerator & rg, DescTable * dt)
{
    const uint32_t desc_table = 10 * static_cast<uint32_t>(collectionHas<SQLTable>(attached_tables));
    const uint32_t desc_view = 10 * static_cast<uint32_t>(collectionHas<SQLView>(attached_views));
    const uint32_t desc_query = 5;
    const uint32_t desc_function = 5;
    const uint32_t desc_system_table = 3 * static_cast<uint32_t>(!systemTables.empty());
    const uint32_t prob_space = desc_table + desc_view + desc_query + desc_function + desc_system_table;
    std::uniform_int_distribution<uint32_t> next_dist(1, prob_space);
    const uint32_t nopt = next_dist(rg.generator);

    if (desc_table && nopt < (desc_table + 1))
    {
        const SQLTable & t = rg.pickRandomly(filterCollection<SQLTable>(attached_tables));

        t.setName(dt->mutable_est(), false);
    }
    else if (desc_view && nopt < (desc_table + desc_view + 1))
    {
        const SQLView & v = rg.pickRandomly(filterCollection<SQLView>(attached_views));

        v.setName(dt->mutable_est(), false);
    }
    else if (desc_query && nopt < (desc_table + desc_view + desc_query + 1))
    {
        this->levels[this->current_level] = QueryLevel(this->current_level);
        generateSelect(rg, false, false, (rg.nextLargeNumber() % 5) + 1, std::numeric_limits<uint32_t>::max(), dt->mutable_sel());
        this->levels.clear();
    }
    else if (desc_function && nopt < (desc_table + desc_view + desc_query + desc_function + 1))
    {
        generateTableFuncCall(rg, dt->mutable_stf());
        this->levels.clear();
    }
    else if (desc_system_table && nopt < (desc_table + desc_view + desc_query + desc_function + desc_system_table + 1))
    {
        ExprSchemaTable * est = dt->mutable_est();

        est->mutable_database()->set_database("system");
        est->mutable_table()->set_table(rg.pickRandomly(systemTables));
    }
    else
    {
        chassert(0);
    }
    if (rg.nextSmallNumber() < 3)
    {
        SettingValues * vals = dt->mutable_setting_values();

        generateSettingValues(rg, serverSettings, vals);
        if (rg.nextSmallNumber() < 3)
        {
            SetValue * sv = vals->add_other_values();

            sv->set_property("describe_include_subcolumns");
            sv->set_value(rg.nextBool() ? "1" : "0");
        }
    }
}

void StatementGenerator::generateNextInsert(RandomGenerator & rg, Insert * ins)
{
    String buf;
    const uint32_t noption = rg.nextLargeNumber();
    const uint32_t noption2 = rg.nextMediumNumber();
    const SQLTable & t = rg.pickRandomly(filterCollection<SQLTable>(attached_tables));
    std::uniform_int_distribution<uint64_t> rows_dist(fc.min_insert_rows, fc.max_insert_rows);
    std::uniform_int_distribution<uint64_t> string_length_dist(1, 8192);
    std::uniform_int_distribution<uint64_t> nested_rows_dist(fc.min_nested_rows, fc.max_nested_rows);

    if (noption2 < 81)
    {
        /// Use insert into table
        t.setName(ins->mutable_est(), false);
    }
    else
    {
        /// Use insert into function
        TableFunction * tf = ins->mutable_tfunc();

        if (fc.clusters.empty() || noption2 < 91)
        {
            setTableRemote(rg, true, t, tf);
        }
        else
        {
            ClusterFunc * cdf = tf->mutable_cluster();

            cdf->set_cname(static_cast<ClusterFunc_CName>((rg.nextRandomUInt32() % static_cast<uint32_t>(ClusterFunc::CName_MAX)) + 1));
            cdf->set_ccluster(rg.pickRandomly(fc.clusters));
            t.setName(cdf->mutable_tof()->mutable_est(), true);
            if (rg.nextBool())
            {
                /// Optional sharding key
                flatTableColumnPath(to_remote_entries, t.cols, [](const SQLColumn &) { return true; });
                cdf->set_sharding_key(rg.pickRandomly(this->remote_entries).getBottomName());
                this->remote_entries.clear();
            }
        }
    }
    flatTableColumnPath(skip_nested_node | flat_nested, t.cols, [](const SQLColumn & c) { return c.canBeInserted(); });
    std::shuffle(this->entries.begin(), this->entries.end(), rg.generator);
    for (const auto & entry : this->entries)
    {
        columnPathRef(entry, ins->add_cols());
    }

    if (noption < 801)
    {
        const uint64_t nrows = rows_dist(rg.generator);

        for (uint64_t i = 0; i < nrows; i++)
        {
            uint64_t j = 0;
            const uint64_t next_nested_rows = nested_rows_dist(rg.generator);

            if (i != 0)
            {
                buf += ", ";
            }
            buf += "(";
            for (const auto & entry : this->entries)
            {
                if (j != 0)
                {
                    buf += ", ";
                }
                if ((entry.dmod.has_value() && entry.dmod.value() == DModifier::DEF_DEFAULT && rg.nextMediumNumber() < 6)
                    || (entry.path.size() == 1 && rg.nextLargeNumber() < 2))
                {
                    buf += "DEFAULT";
                }
                else if (entry.special == ColumnSpecial::SIGN)
                {
                    buf += rg.nextBool() ? "1" : "-1";
                }
                else if (entry.special == ColumnSpecial::IS_DELETED)
                {
                    buf += rg.nextBool() ? "1" : "0";
                }
                else if (entry.path.size() > 1)
                {
                    /// Make sure all nested entries have the same number of rows
                    buf += ArrayType::appendRandomRawValue(rg, *this, entry.getBottomType(), next_nested_rows);
                }
                else
                {
                    buf += strAppendAnyValue(rg, entry.getBottomType());
                }
                j++;
            }
            buf += ")";
        }
        ins->set_query(buf);
    }
    else if (noption < 951)
    {
        Select * sel = ins->mutable_select();

        if (noption < 901)
        {
            /// Use generateRandom
            bool first = true;
            SelectStatementCore * ssc = sel->mutable_select_core();
            GenerateRandomFunc * grf = ssc->mutable_from()
                                           ->mutable_tos()
                                           ->mutable_join_clause()
                                           ->mutable_tos()
                                           ->mutable_joined_table()
                                           ->mutable_tof()
                                           ->mutable_tfunc()
                                           ->mutable_grandom();

            for (const auto & entry : this->entries)
            {
                SQLType * tp = entry.getBottomType();
                const String & bottomName = entry.getBottomName();

                buf += fmt::format(
                    "{}{} {}{}{}",
                    first ? "" : ", ",
                    bottomName,
                    entry.path.size() > 1 ? "Array(" : "",
                    tp->typeName(false),
                    entry.path.size() > 1 ? ")" : "");
                ssc->add_result_columns()->mutable_etc()->mutable_col()->mutable_path()->mutable_col()->set_column(bottomName);
                first = false;
            }
            grf->mutable_structure()->mutable_lit_val()->set_string_lit(std::move(buf));
            grf->set_random_seed(rg.nextRandomUInt64());
            grf->set_max_string_length(string_length_dist(rg.generator));
            grf->set_max_array_length(nested_rows_dist(rg.generator));
            ssc->mutable_limit()->mutable_limit()->mutable_lit_val()->mutable_int_lit()->set_uint_lit(rows_dist(rg.generator));
        }
        else
        {
            this->levels[this->current_level] = QueryLevel(this->current_level);
            if (rg.nextMediumNumber() < 13)
            {
                this->addCTEs(rg, std::numeric_limits<uint32_t>::max(), ins->mutable_ctes());
            }
            generateSelect(rg, true, false, static_cast<uint32_t>(this->entries.size()), std::numeric_limits<uint32_t>::max(), sel);
            this->levels.clear();
        }
    }
    else
    {
        const uint32_t nrows = (rg.nextSmallNumber() % 3) + 1;
        ValuesStatement * vs = ins->mutable_values();

        this->levels[this->current_level] = QueryLevel(this->current_level);
        this->levels[this->current_level].allow_aggregates = this->levels[this->current_level].allow_window_funcs = false;
        for (uint32_t i = 0; i < nrows; i++)
        {
            bool first = true;
            ExprList * elist = i == 0 ? vs->mutable_expr_list() : vs->add_extra_expr_lists();

            for (const auto & entry : this->entries)
            {
                Expr * expr = first ? elist->mutable_expr() : elist->add_extra_exprs();

                if (entry.special == ColumnSpecial::SIGN)
                {
                    expr->mutable_lit_val()->mutable_int_lit()->set_int_lit(rg.nextBool() ? 1 : -1);
                }
                else if (entry.special == ColumnSpecial::IS_DELETED)
                {
                    expr->mutable_lit_val()->mutable_int_lit()->set_int_lit(rg.nextBool() ? 1 : 0);
                }
                else
                {
                    generateExpression(rg, expr);
                }
                first = false;
            }
        }
        this->levels.clear();
    }
    this->entries.clear();
    if (rg.nextSmallNumber() < 3)
    {
        generateSettingValues(rg, serverSettings, ins->mutable_setting_values());
    }
}

void StatementGenerator::generateUptDelWhere(RandomGenerator & rg, const SQLTable & t, Expr * expr)
{
    if (rg.nextSmallNumber() < 10)
    {
        addTableRelation(rg, true, "", t);
        this->levels[this->current_level].allow_aggregates = this->levels[this->current_level].allow_window_funcs = false;
        generateWherePredicate(rg, expr);
        this->levels.clear();
    }
    else
    {
        expr->mutable_lit_val()->set_special_val(SpecialVal::VAL_TRUE);
    }
}

void StatementGenerator::generateNextDelete(RandomGenerator & rg, LightDelete * del)
{
    const SQLTable & t = rg.pickRandomly(filterCollection<SQLTable>(attached_tables));
    const std::optional<String> cluster = t.getCluster();

    t.setName(del->mutable_est(), false);
    if (cluster.has_value())
    {
        del->mutable_cluster()->set_cluster(cluster.value());
    }
    if (t.isMergeTreeFamily() && rg.nextBool())
    {
        generateNextTablePartition(rg, false, t, del->mutable_single_partition()->mutable_partition());
    }
    generateUptDelWhere(rg, t, del->mutable_where()->mutable_expr()->mutable_expr());
    if (rg.nextSmallNumber() < 3)
    {
        generateSettingValues(rg, serverSettings, del->mutable_setting_values());
    }
}

void StatementGenerator::generateNextTruncate(RandomGenerator & rg, Truncate * trunc)
{
    const bool trunc_database = collectionHas<std::shared_ptr<SQLDatabase>>(attached_databases);
    const uint32_t trunc_table = 980 * static_cast<uint32_t>(collectionHas<SQLTable>(attached_tables));
    const uint32_t trunc_db_tables = 15 * static_cast<uint32_t>(trunc_database);
    const uint32_t trunc_db = 5 * static_cast<uint32_t>(trunc_database);
    const uint32_t prob_space = trunc_table + trunc_db_tables + trunc_db;
    std::uniform_int_distribution<uint32_t> next_dist(1, prob_space);
    const uint32_t nopt = next_dist(rg.generator);
    std::optional<String> cluster;

    if (trunc_table && nopt < (trunc_table + 1))
    {
        const SQLTable & t = rg.pickRandomly(filterCollection<SQLTable>(attached_tables));

        cluster = t.getCluster();
        t.setName(trunc->mutable_est(), false);
    }
    else if (trunc_db_tables && nopt < (trunc_table + trunc_db_tables + 1))
    {
        const std::shared_ptr<SQLDatabase> & d = rg.pickRandomly(filterCollection<std::shared_ptr<SQLDatabase>>(attached_databases));

        cluster = d->getCluster();
        d->setName(trunc->mutable_all_tables());
    }
    else if (trunc_db && nopt < (trunc_table + trunc_db_tables + trunc_db + 1))
    {
        const std::shared_ptr<SQLDatabase> & d = rg.pickRandomly(filterCollection<std::shared_ptr<SQLDatabase>>(attached_databases));

        cluster = d->getCluster();
        d->setName(trunc->mutable_database());
    }
    else
    {
        chassert(0);
    }
    if (cluster.has_value())
    {
        trunc->mutable_cluster()->set_cluster(cluster.value());
    }
    trunc->set_sync(rg.nextSmallNumber() < 4);
    if (rg.nextSmallNumber() < 3)
    {
        generateSettingValues(rg, serverSettings, trunc->mutable_setting_values());
    }
}

static const auto exchange_table_lambda = [](const SQLTable & t)
{
    /// I would need to track the table clusters to do this correctly, ie ensure tables to be exchanged are on same cluster
    return t.isAttached() && !t.hasDatabasePeer() && !t.getCluster();
};

void StatementGenerator::generateNextExchangeTables(RandomGenerator & rg, ExchangeTables * et)
{
    const auto & input = filterCollection<SQLTable>(exchange_table_lambda);

    for (const auto & entry : input)
    {
        this->ids.push_back(entry.get().tname);
    }
    std::shuffle(this->ids.begin(), this->ids.end(), rg.generator);
    const SQLTable & t1 = this->tables[this->ids[0]];
    const SQLTable & t2 = this->tables[this->ids[1]];

    t1.setName(et->mutable_est1(), false);
    t2.setName(et->mutable_est2(), false);
    this->ids.clear();
    if (t1.cluster.has_value() && t2.cluster.has_value() && t1.cluster == t2.cluster)
    {
        et->mutable_cluster()->set_cluster(t1.cluster.value());
    }
    if (rg.nextSmallNumber() < 3)
    {
        generateSettingValues(rg, serverSettings, et->mutable_setting_values());
    }
}

static const auto alter_table_lambda = [](const SQLTable & t) { return t.isAttached() && !t.isFileEngine(); };

void StatementGenerator::generateAlterTable(RandomGenerator & rg, AlterTable * at)
{
    ExprSchemaTable * est = at->mutable_est();
    const uint32_t nalters = rg.nextBool() ? 1 : ((rg.nextMediumNumber() % 4) + 1);
    const bool has_tables = collectionHas<SQLTable>(alter_table_lambda);
    const bool has_views = collectionHas<SQLView>(attached_views);
    std::optional<String> cluster;

    if (has_views && (!has_tables || rg.nextBool()))
    {
        SQLView & v = const_cast<SQLView &>(rg.pickRandomly(filterCollection<SQLView>(attached_views)).get());

        cluster = v.getCluster();
        v.setName(est, false);
        for (uint32_t i = 0; i < nalters; i++)
        {
            const uint32_t alter_refresh = 1 * static_cast<uint32_t>(v.is_refreshable);
            const uint32_t alter_query = 3;
            const uint32_t prob_space = alter_refresh + alter_query;
            AlterTableItem * ati = i == 0 ? at->mutable_alter() : at->add_other_alters();
            std::uniform_int_distribution<uint32_t> next_dist(1, prob_space);
            const uint32_t nopt = next_dist(rg.generator);

            if (alter_refresh && nopt < (alter_refresh + 1))
            {
                generateNextRefreshableView(rg, ati->mutable_refresh());
            }
            else
            {
                v.staged_ncols
                    = v.has_with_cols ? static_cast<uint32_t>(v.cols.size()) : ((rg.nextMediumNumber() % fc.max_columns) + UINT32_C(1));

                if (v.is_deterministic)
                {
                    this->setAllowNotDetermistic(false);
                    this->enforceFinal(true);
                }
                this->levels[this->current_level] = QueryLevel(this->current_level);
                this->allow_in_expression_alias = rg.nextSmallNumber() < 3;
                generateSelect(
                    rg,
                    false,
                    false,
                    v.staged_ncols,
                    v.is_materialized ? (~allow_prewhere) : std::numeric_limits<uint32_t>::max(),
                    ati->mutable_modify_query());
                this->levels.clear();
                this->allow_in_expression_alias = true;
                if (v.is_deterministic)
                {
                    this->setAllowNotDetermistic(true);
                    this->enforceFinal(false);
                }
                matchQueryAliases(v, ati->release_modify_query(), ati->mutable_modify_query());
            }
        }
    }
    else if (has_tables)
    {
        SQLTable & t = const_cast<SQLTable &>(rg.pickRandomly(filterCollection<SQLTable>(alter_table_lambda)).get());
        const String dname = t.db ? ("d" + std::to_string(t.db->dname)) : "";
        const String tname = "t" + std::to_string(t.tname);
        const bool table_has_partitions = t.isMergeTreeFamily() && fc.tableHasPartitions(false, dname, tname);

        cluster = t.getCluster();
        at->set_is_temp(t.is_temp);
        t.setName(est, false);
        for (uint32_t i = 0; i < nalters; i++)
        {
            const uint32_t alter_order_by = 3 * static_cast<uint32_t>(t.isMergeTreeFamily());
            const uint32_t heavy_delete = 30;
            const uint32_t heavy_update = 40;
            const uint32_t add_column = 2 * static_cast<uint32_t>(!t.hasDatabasePeer() && t.cols.size() < 10);
            const uint32_t materialize_column = 2;
            const uint32_t drop_column = 2 * static_cast<uint32_t>(!t.hasDatabasePeer() && t.cols.size() > 1);
            const uint32_t rename_column = 2 * static_cast<uint32_t>(!t.hasDatabasePeer());
            const uint32_t clear_column = 2;
            const uint32_t modify_column = 2 * static_cast<uint32_t>(!t.hasDatabasePeer());
            const uint32_t comment_column = 2;
            const uint32_t add_stats = 3 * static_cast<uint32_t>(t.isMergeTreeFamily());
            const uint32_t mod_stats = 3 * static_cast<uint32_t>(t.isMergeTreeFamily());
            const uint32_t drop_stats = 3 * static_cast<uint32_t>(t.isMergeTreeFamily());
            const uint32_t clear_stats = 3 * static_cast<uint32_t>(t.isMergeTreeFamily());
            const uint32_t mat_stats = 3 * static_cast<uint32_t>(t.isMergeTreeFamily());
            const uint32_t delete_mask = 8 * static_cast<uint32_t>(t.isMergeTreeFamily());
            const uint32_t add_idx = 2 * static_cast<uint32_t>(t.idxs.size() < 3);
            const uint32_t materialize_idx = 2 * static_cast<uint32_t>(!t.idxs.empty());
            const uint32_t clear_idx = 2 * static_cast<uint32_t>(!t.idxs.empty());
            const uint32_t drop_idx = 2 * static_cast<uint32_t>(!t.idxs.empty());
            const uint32_t column_remove_property = 2;
            const uint32_t column_modify_setting = 2 * static_cast<uint32_t>(!allColumnSettings.at(t.teng).empty());
            const uint32_t column_remove_setting = 2 * static_cast<uint32_t>(!allColumnSettings.at(t.teng).empty());
            const uint32_t table_modify_setting = 2;
            const uint32_t table_remove_setting = 2;
            const uint32_t add_projection = 2 * static_cast<uint32_t>(t.isMergeTreeFamily());
            const uint32_t remove_projection = 2 * static_cast<uint32_t>(t.isMergeTreeFamily() && !t.projs.empty());
            const uint32_t materialize_projection = 2 * static_cast<uint32_t>(t.isMergeTreeFamily() && !t.projs.empty());
            const uint32_t clear_projection = 2 * static_cast<uint32_t>(t.isMergeTreeFamily() && !t.projs.empty());
            const uint32_t add_constraint = 2 * static_cast<uint32_t>(t.constrs.size() < 4);
            const uint32_t remove_constraint = 2 * static_cast<uint32_t>(!t.constrs.empty());
            const uint32_t detach_partition = 5 * static_cast<uint32_t>(t.isMergeTreeFamily());
            const uint32_t drop_partition = 5 * static_cast<uint32_t>(t.isMergeTreeFamily());
            const uint32_t drop_detached_partition = 5 * static_cast<uint32_t>(t.isMergeTreeFamily());
            const uint32_t forget_partition = 5 * static_cast<uint32_t>(table_has_partitions);
            const uint32_t attach_partition = 5 * static_cast<uint32_t>(t.isMergeTreeFamily());
            const uint32_t move_partition_to = 5 * static_cast<uint32_t>(table_has_partitions);
            const uint32_t clear_column_partition = 5 * static_cast<uint32_t>(table_has_partitions);
            const uint32_t freeze_partition = 5 * static_cast<uint32_t>(t.isMergeTreeFamily());
            const uint32_t unfreeze_partition = 7 * static_cast<uint32_t>(!t.frozen_partitions.empty());
            const uint32_t clear_index_partition = 5 * static_cast<uint32_t>(table_has_partitions && !t.idxs.empty());
            const uint32_t move_partition = 5 * static_cast<uint32_t>(table_has_partitions && !fc.disks.empty());
            const uint32_t modify_ttl = 5 * static_cast<uint32_t>(t.isMergeTreeFamily() && !t.hasDatabasePeer());
            const uint32_t remove_ttl = 2 * static_cast<uint32_t>(t.isMergeTreeFamily() && !t.hasDatabasePeer());
            const uint32_t comment_table = 2;
            const uint32_t prob_space = alter_order_by + heavy_delete + heavy_update + add_column + materialize_column + drop_column
                + rename_column + clear_column + modify_column + comment_column + delete_mask + add_stats + mod_stats + drop_stats
                + clear_stats + mat_stats + add_idx + materialize_idx + clear_idx + drop_idx + column_remove_property
                + column_modify_setting + column_remove_setting + table_modify_setting + table_remove_setting + add_projection
                + remove_projection + materialize_projection + clear_projection + add_constraint + remove_constraint + detach_partition
                + drop_partition + drop_detached_partition + forget_partition + attach_partition + move_partition_to
                + clear_column_partition + freeze_partition + unfreeze_partition + clear_index_partition + move_partition + modify_ttl
                + remove_ttl + comment_table;
            AlterTableItem * ati = i == 0 ? at->mutable_alter() : at->add_other_alters();
            std::uniform_int_distribution<uint32_t> next_dist(1, prob_space);
            const uint32_t nopt = next_dist(rg.generator);

            if (alter_order_by && nopt < (alter_order_by + 1))
            {
                TableKey * tkey = ati->mutable_order();

                if (rg.nextSmallNumber() < 6)
                {
                    flatTableColumnPath(
                        flat_tuple | flat_nested | flat_json | skip_nested_node, t.cols, [](const SQLColumn &) { return true; });
                    generateTableKey(rg, t.teng, true, tkey);
                    this->entries.clear();
                    this->levels.clear();
                }
            }
            else if (heavy_delete && nopt < (heavy_delete + alter_order_by + 1))
            {
                HeavyDelete * hdel = ati->mutable_del();

                if (t.isMergeTreeFamily() && rg.nextBool())
                {
                    generateNextTablePartition(rg, false, t, hdel->mutable_single_partition()->mutable_partition());
                }
                generateUptDelWhere(rg, t, hdel->mutable_del()->mutable_expr()->mutable_expr());
            }
            else if (add_column && nopt < (heavy_delete + alter_order_by + add_column + 1))
            {
                const uint32_t next_option = rg.nextSmallNumber();
                AddColumn * add_col = ati->mutable_add_column();

                addTableColumn(
                    rg, t, t.col_counter++, true, false, rg.nextMediumNumber() < 6, ColumnSpecial::NONE, add_col->mutable_new_col());
                if (next_option < 4)
                {
                    flatTableColumnPath(flat_tuple | flat_nested, t.cols, [](const SQLColumn &) { return true; });
                    columnPathRef(rg.pickRandomly(this->entries), add_col->mutable_add_where()->mutable_col());
                    this->entries.clear();
                }
                else if (next_option < 8)
                {
                    add_col->mutable_add_where()->set_first(true);
                }
            }
            else if (materialize_column && nopt < (heavy_delete + alter_order_by + add_column + materialize_column + 1))
            {
                ColInPartition * mcol = ati->mutable_materialize_column();

                flatTableColumnPath(flat_nested, t.cols, [](const SQLColumn &) { return true; });
                columnPathRef(rg.pickRandomly(this->entries), mcol->mutable_col());
                this->entries.clear();
                if (t.isMergeTreeFamily() && rg.nextBool())
                {
                    generateNextTablePartition(rg, false, t, mcol->mutable_single_partition()->mutable_partition());
                }
            }
            else if (drop_column && nopt < (heavy_delete + alter_order_by + add_column + materialize_column + drop_column + 1))
            {
                flatTableColumnPath(flat_nested, t.cols, [](const SQLColumn &) { return true; });
                columnPathRef(rg.pickRandomly(this->entries), ati->mutable_drop_column());
                this->entries.clear();
            }
            else if (
                rename_column && nopt < (heavy_delete + alter_order_by + add_column + materialize_column + drop_column + rename_column + 1))
            {
                const uint32_t ncname = t.col_counter++;
                RenameCol * rcol = ati->mutable_rename_column();

                flatTableColumnPath(flat_nested, t.cols, [](const SQLColumn &) { return true; });
                columnPathRef(rg.pickRandomly(this->entries), rcol->mutable_old_name());
                this->entries.clear();

                rcol->mutable_new_name()->CopyFrom(rcol->old_name());
                const uint32_t size = rcol->new_name().sub_cols_size();
                Column & ncol = const_cast<Column &>(size ? rcol->new_name().sub_cols(size - 1) : rcol->new_name().col());
                ncol.set_column("c" + std::to_string(ncname));
            }
            else if (
                clear_column
                && nopt
                    < (heavy_delete + alter_order_by + add_column + materialize_column + drop_column + rename_column + clear_column + 1))
            {
                ColInPartition * ccol = ati->mutable_clear_column();

                flatTableColumnPath(flat_nested, t.cols, [](const SQLColumn &) { return true; });
                columnPathRef(rg.pickRandomly(this->entries), ccol->mutable_col());
                this->entries.clear();
                if (t.isMergeTreeFamily() && rg.nextBool())
                {
                    generateNextTablePartition(rg, false, t, ccol->mutable_single_partition()->mutable_partition());
                }
            }
            else if (
                modify_column
                && nopt
                    < (heavy_delete + alter_order_by + add_column + materialize_column + drop_column + rename_column + clear_column
                       + modify_column + 1))
            {
                const uint32_t next_option = rg.nextSmallNumber();
                AddColumn * add_col = ati->mutable_modify_column();

                addTableColumn(
                    rg, t, rg.pickRandomly(t.cols), true, true, rg.nextMediumNumber() < 6, ColumnSpecial::NONE, add_col->mutable_new_col());
                if (next_option < 4)
                {
                    flatTableColumnPath(flat_tuple | flat_nested, t.cols, [](const SQLColumn &) { return true; });
                    columnPathRef(rg.pickRandomly(this->entries), add_col->mutable_add_where()->mutable_col());
                    this->entries.clear();
                }
                else if (next_option < 8)
                {
                    add_col->mutable_add_where()->set_first(true);
                }
            }
            else if (
                comment_column
                && nopt
                    < (heavy_delete + alter_order_by + add_column + materialize_column + drop_column + rename_column + clear_column
                       + modify_column + comment_column + 1))
            {
                CommentColumn * ccol = ati->mutable_comment_column();

                flatTableColumnPath(flat_nested, t.cols, [](const SQLColumn &) { return true; });
                columnPathRef(rg.pickRandomly(this->entries), ccol->mutable_col());
                this->entries.clear();
                ccol->set_comment(rg.nextString("'", true, rg.nextRandomUInt32() % 1009));
            }
            else if (
                delete_mask
                && nopt
                    < (heavy_delete + alter_order_by + add_column + materialize_column + drop_column + rename_column + clear_column
                       + modify_column + comment_column + delete_mask + 1))
            {
                ApplyDeleteMask * adm = ati->mutable_delete_mask();

                if (rg.nextBool())
                {
                    generateNextTablePartition(rg, false, t, adm->mutable_single_partition()->mutable_partition());
                }
            }
            else if (
                heavy_update
                && nopt
                    < (heavy_delete + alter_order_by + add_column + materialize_column + drop_column + rename_column + clear_column
                       + modify_column + comment_column + delete_mask + heavy_update + 1))
            {
                Update * upt = ati->mutable_update();

                if (t.isMergeTreeFamily() && rg.nextBool())
                {
                    generateNextTablePartition(rg, false, t, upt->mutable_single_partition()->mutable_partition());
                }
                flatTableColumnPath(0, t.cols, [](const SQLColumn & c) { return c.tp->getTypeClass() != SQLTypeClass::NESTED; });
                if (this->entries.empty())
                {
                    UpdateSet * upset = upt->mutable_update();

                    upset->mutable_col()->mutable_col()->set_column("c0");
                    upset->mutable_expr()->mutable_lit_val()->mutable_int_lit()->set_int_lit(0);
                }
                else
                {
                    const uint32_t nupdates
                        = (rg.nextMediumNumber() % std::min<uint32_t>(static_cast<uint32_t>(this->entries.size()), UINT32_C(4))) + 1;

                    std::shuffle(this->entries.begin(), this->entries.end(), rg.generator);
                    for (uint32_t j = 0; j < nupdates; j++)
                    {
                        columnPathRef(
                            this->entries[j], j == 0 ? upt->mutable_update()->mutable_col() : upt->add_other_updates()->mutable_col());
                    }
                    addTableRelation(rg, true, "", t);
                    this->levels[this->current_level].allow_aggregates = this->levels[this->current_level].allow_window_funcs = false;
                    for (uint32_t j = 0; j < nupdates; j++)
                    {
                        const ColumnPathChain & entry = this->entries[j];
                        UpdateSet & uset = const_cast<UpdateSet &>(j == 0 ? upt->update() : upt->other_updates(j - 1));
                        Expr * expr = uset.mutable_expr();

                        if (rg.nextSmallNumber() < 9)
                        {
                            /// Set constant value
                            String buf;
                            LiteralValue * lv = expr->mutable_lit_val();

                            if ((entry.dmod.has_value() && entry.dmod.value() == DModifier::DEF_DEFAULT && rg.nextMediumNumber() < 6)
                                || (entry.path.size() == 1 && rg.nextLargeNumber() < 2))
                            {
                                buf = "DEFAULT";
                            }
                            else if (entry.special == ColumnSpecial::SIGN)
                            {
                                buf = rg.nextBool() ? "1" : "-1";
                            }
                            else if (entry.special == ColumnSpecial::IS_DELETED)
                            {
                                buf = rg.nextBool() ? "1" : "0";
                            }
                            else
                            {
                                buf = strAppendAnyValue(rg, entry.getBottomType());
                            }
                            lv->set_no_quote_str(buf);
                        }
                        else
                        {
                            generateExpression(rg, expr);
                        }
                    }
                    this->levels.clear();
                    this->entries.clear();
                }

                generateUptDelWhere(rg, t, upt->mutable_where()->mutable_expr()->mutable_expr());
            }
            else if (
                add_stats
                && nopt
                    < (heavy_delete + alter_order_by + add_column + materialize_column + drop_column + rename_column + clear_column
                       + modify_column + comment_column + delete_mask + heavy_update + add_stats + 1))
            {
                AddStatistics * ads = ati->mutable_add_stats();

                pickUpNextCols(rg, t, ads->mutable_cols());
                generateNextStatistics(rg, ads->mutable_stats());
            }
            else if (
                mod_stats
                && nopt
                    < (heavy_delete + alter_order_by + add_column + materialize_column + drop_column + rename_column + clear_column
                       + modify_column + comment_column + delete_mask + heavy_update + add_stats + mod_stats + 1))
            {
                AddStatistics * ads = ati->mutable_mod_stats();

                pickUpNextCols(rg, t, ads->mutable_cols());
                generateNextStatistics(rg, ads->mutable_stats());
            }
            else if (
                drop_stats
                && nopt
                    < (heavy_delete + alter_order_by + add_column + materialize_column + drop_column + rename_column + clear_column
                       + modify_column + comment_column + delete_mask + heavy_update + add_stats + mod_stats + drop_stats + 1))
            {
                pickUpNextCols(rg, t, ati->mutable_drop_stats());
            }
            else if (
                clear_stats
                && nopt
                    < (heavy_delete + alter_order_by + add_column + materialize_column + drop_column + rename_column + clear_column
                       + modify_column + comment_column + delete_mask + heavy_update + add_stats + mod_stats + drop_stats + clear_stats
                       + 1))
            {
                pickUpNextCols(rg, t, ati->mutable_clear_stats());
            }
            else if (
                mat_stats
                && nopt
                    < (heavy_delete + alter_order_by + add_column + materialize_column + drop_column + rename_column + clear_column
                       + modify_column + comment_column + delete_mask + heavy_update + add_stats + mod_stats + drop_stats + clear_stats
                       + mat_stats + 1))
            {
                pickUpNextCols(rg, t, ati->mutable_mat_stats());
            }
            else if (
                add_idx
                && nopt
                    < (heavy_delete + alter_order_by + add_column + materialize_column + drop_column + rename_column + clear_column
                       + modify_column + comment_column + delete_mask + heavy_update + add_stats + mod_stats + drop_stats + clear_stats
                       + mat_stats + add_idx + 1))
            {
                AddIndex * add_index = ati->mutable_add_index();

                addTableIndex(rg, t, true, add_index->mutable_new_idx());
                if (!t.idxs.empty())
                {
                    const uint32_t next_option = rg.nextSmallNumber();

                    if (next_option < 4)
                    {
                        add_index->mutable_add_where()->mutable_idx()->set_index("i" + std::to_string(rg.pickRandomly(t.idxs)));
                    }
                    else if (next_option < 8)
                    {
                        add_index->mutable_add_where()->set_first(true);
                    }
                }
            }
            else if (
                materialize_idx
                && nopt
                    < (heavy_delete + alter_order_by + add_column + materialize_column + drop_column + rename_column + clear_column
                       + modify_column + comment_column + delete_mask + heavy_update + add_stats + mod_stats + drop_stats + clear_stats
                       + mat_stats + add_idx + materialize_idx + 1))
            {
                IdxInPartition * iip = ati->mutable_materialize_index();

                iip->mutable_idx()->set_index("i" + std::to_string(rg.pickRandomly(t.idxs)));
                if (t.isMergeTreeFamily() && rg.nextBool())
                {
                    generateNextTablePartition(rg, false, t, iip->mutable_single_partition()->mutable_partition());
                }
            }
            else if (
                clear_idx
                && nopt
                    < (heavy_delete + alter_order_by + add_column + materialize_column + drop_column + rename_column + clear_column
                       + modify_column + comment_column + delete_mask + heavy_update + add_stats + mod_stats + drop_stats + clear_stats
                       + mat_stats + add_idx + materialize_idx + clear_idx + 1))
            {
                IdxInPartition * iip = ati->mutable_clear_index();

                iip->mutable_idx()->set_index("i" + std::to_string(rg.pickRandomly(t.idxs)));
                if (t.isMergeTreeFamily() && rg.nextBool())
                {
                    generateNextTablePartition(rg, false, t, iip->mutable_single_partition()->mutable_partition());
                }
            }
            else if (
                drop_idx
                && nopt
                    < (heavy_delete + alter_order_by + add_column + materialize_column + drop_column + rename_column + clear_column
                       + modify_column + comment_column + delete_mask + heavy_update + add_stats + mod_stats + drop_stats + clear_stats
                       + mat_stats + add_idx + materialize_idx + clear_idx + drop_idx + 1))
            {
                ati->mutable_drop_index()->set_index("i" + std::to_string(rg.pickRandomly(t.idxs)));
            }
            else if (
                column_remove_property
                && nopt
                    < (heavy_delete + alter_order_by + add_column + materialize_column + drop_column + rename_column + clear_column
                       + modify_column + comment_column + delete_mask + heavy_update + add_stats + mod_stats + drop_stats + clear_stats
                       + mat_stats + add_idx + materialize_idx + clear_idx + drop_idx + column_remove_property + 1))
            {
                RemoveColumnProperty * rcs = ati->mutable_column_remove_property();

                flatTableColumnPath(flat_nested, t.cols, [](const SQLColumn &) { return true; });
                columnPathRef(rg.pickRandomly(this->entries), rcs->mutable_col());
                this->entries.clear();
                rcs->set_property(static_cast<RemoveColumnProperty_ColumnProperties>(
                    (rg.nextRandomUInt32() % static_cast<uint32_t>(RemoveColumnProperty::ColumnProperties_MAX)) + 1));
            }
            else if (
                column_modify_setting
                && nopt
                    < (heavy_delete + alter_order_by + add_column + materialize_column + drop_column + rename_column + clear_column
                       + modify_column + comment_column + delete_mask + heavy_update + add_stats + mod_stats + drop_stats + clear_stats
                       + mat_stats + add_idx + materialize_idx + clear_idx + drop_idx + column_remove_property + column_modify_setting + 1))
            {
                ModifyColumnSetting * mcp = ati->mutable_column_modify_setting();
                const auto & csettings = allColumnSettings.at(t.teng);

                flatTableColumnPath(flat_nested, t.cols, [](const SQLColumn &) { return true; });
                columnPathRef(rg.pickRandomly(this->entries), mcp->mutable_col());
                this->entries.clear();
                generateSettingValues(rg, csettings, mcp->mutable_setting_values());
            }
            else if (
                column_remove_setting
                && nopt
                    < (heavy_delete + alter_order_by + add_column + materialize_column + drop_column + rename_column + clear_column
                       + modify_column + comment_column + delete_mask + heavy_update + add_stats + mod_stats + drop_stats + clear_stats
                       + mat_stats + add_idx + materialize_idx + clear_idx + drop_idx + column_remove_property + column_modify_setting
                       + column_remove_setting + 1))
            {
                RemoveColumnSetting * rcp = ati->mutable_column_remove_setting();
                const auto & csettings = allColumnSettings.at(t.teng);

                flatTableColumnPath(flat_nested, t.cols, [](const SQLColumn &) { return true; });
                columnPathRef(rg.pickRandomly(this->entries), rcp->mutable_col());
                this->entries.clear();
                generateSettingList(rg, csettings, rcp->mutable_setting_values());
            }
            else if (
                table_modify_setting
                && nopt
                    < (heavy_delete + alter_order_by + add_column + materialize_column + drop_column + rename_column + clear_column
                       + modify_column + comment_column + delete_mask + heavy_update + add_stats + mod_stats + drop_stats + clear_stats
                       + mat_stats + add_idx + materialize_idx + clear_idx + drop_idx + column_remove_property + column_modify_setting
                       + column_remove_setting + table_modify_setting + 1))
            {
                SettingValues * svs = ati->mutable_table_modify_setting();
                const auto & engineSettings = allTableSettings.at(t.teng);

                if (!engineSettings.empty() && rg.nextSmallNumber() < 9)
                {
                    /// Modify table engine settings
                    generateSettingValues(rg, engineSettings, svs);
                }
                if (!svs->has_set_value() || rg.nextSmallNumber() < 4)
                {
                    /// Modify server settings
                    generateSettingValues(rg, serverSettings, svs);
                }
            }
            else if (
                table_remove_setting
                && nopt
                    < (heavy_delete + alter_order_by + add_column + materialize_column + drop_column + rename_column + clear_column
                       + modify_column + comment_column + delete_mask + heavy_update + add_stats + mod_stats + drop_stats + clear_stats
                       + mat_stats + add_idx + materialize_idx + clear_idx + drop_idx + column_remove_property + column_modify_setting
                       + column_remove_setting + table_modify_setting + table_remove_setting + 1))
            {
                SettingList * sl = ati->mutable_table_remove_setting();
                const auto & engineSettings = allTableSettings.at(t.teng);

                if (!engineSettings.empty() && rg.nextSmallNumber() < 9)
                {
                    /// Remove table engine settings
                    generateSettingList(rg, engineSettings, sl);
                }
                if (!sl->has_setting() || rg.nextSmallNumber() < 4)
                {
                    /// Remove server settings
                    generateSettingList(rg, serverSettings, sl);
                }
            }
            else if (
                add_projection
                && nopt
                    < (heavy_delete + alter_order_by + add_column + materialize_column + drop_column + rename_column + clear_column
                       + modify_column + comment_column + delete_mask + heavy_update + add_stats + mod_stats + drop_stats + clear_stats
                       + mat_stats + add_idx + materialize_idx + clear_idx + drop_idx + column_remove_property + column_modify_setting
                       + column_remove_setting + table_modify_setting + table_remove_setting + add_projection + 1))
            {
                addTableProjection(rg, t, true, ati->mutable_add_projection());
            }
            else if (
                remove_projection
                && nopt
                    < (heavy_delete + alter_order_by + add_column + materialize_column + drop_column + rename_column + clear_column
                       + modify_column + comment_column + delete_mask + heavy_update + add_stats + mod_stats + drop_stats + clear_stats
                       + mat_stats + add_idx + materialize_idx + clear_idx + drop_idx + column_remove_property + column_modify_setting
                       + column_remove_setting + table_modify_setting + table_remove_setting + add_projection + remove_projection + 1))
            {
                ati->mutable_remove_projection()->set_projection("p" + std::to_string(rg.pickRandomly(t.projs)));
            }
            else if (
                materialize_projection
                && nopt
                    < (heavy_delete + alter_order_by + add_column + materialize_column + drop_column + rename_column + clear_column
                       + modify_column + comment_column + delete_mask + heavy_update + add_stats + mod_stats + drop_stats + clear_stats
                       + mat_stats + add_idx + materialize_idx + clear_idx + drop_idx + column_remove_property + column_modify_setting
                       + column_remove_setting + table_modify_setting + table_remove_setting + add_projection + remove_projection
                       + materialize_projection + 1))
            {
                ProjectionInPartition * pip = ati->mutable_materialize_projection();

                pip->mutable_proj()->set_projection("p" + std::to_string(rg.pickRandomly(t.projs)));
                if (t.isMergeTreeFamily() && rg.nextBool())
                {
                    generateNextTablePartition(rg, false, t, pip->mutable_single_partition()->mutable_partition());
                }
            }
            else if (
                clear_projection
                && nopt
                    < (heavy_delete + alter_order_by + add_column + materialize_column + drop_column + rename_column + clear_column
                       + modify_column + comment_column + delete_mask + heavy_update + add_stats + mod_stats + drop_stats + clear_stats
                       + mat_stats + add_idx + materialize_idx + clear_idx + drop_idx + column_remove_property + column_modify_setting
                       + column_remove_setting + table_modify_setting + table_remove_setting + add_projection + remove_projection
                       + materialize_projection + clear_projection + 1))
            {
                ProjectionInPartition * pip = ati->mutable_clear_projection();

                pip->mutable_proj()->set_projection("p" + std::to_string(rg.pickRandomly(t.projs)));
                if (t.isMergeTreeFamily() && rg.nextBool())
                {
                    generateNextTablePartition(rg, false, t, pip->mutable_single_partition()->mutable_partition());
                }
            }
            else if (
                add_constraint
                && nopt
                    < (heavy_delete + alter_order_by + add_column + materialize_column + drop_column + rename_column + clear_column
                       + modify_column + comment_column + delete_mask + heavy_update + add_stats + mod_stats + drop_stats + clear_stats
                       + mat_stats + add_idx + materialize_idx + clear_idx + drop_idx + column_remove_property + column_modify_setting
                       + column_remove_setting + table_modify_setting + table_remove_setting + add_projection + remove_projection
                       + materialize_projection + clear_projection + add_constraint + 1))
            {
                addTableConstraint(rg, t, true, ati->mutable_add_constraint());
            }
            else if (
                remove_constraint
                && nopt
                    < (heavy_delete + alter_order_by + add_column + materialize_column + drop_column + rename_column + clear_column
                       + modify_column + comment_column + delete_mask + heavy_update + add_stats + mod_stats + drop_stats + clear_stats
                       + mat_stats + add_idx + materialize_idx + clear_idx + drop_idx + column_remove_property + column_modify_setting
                       + column_remove_setting + table_modify_setting + table_remove_setting + add_projection + remove_projection
                       + materialize_projection + clear_projection + add_constraint + remove_constraint + 1))
            {
                ati->mutable_remove_constraint()->set_constraint("c" + std::to_string(rg.pickRandomly(t.constrs)));
            }
            else if (
                detach_partition
                && nopt
                    < (heavy_delete + alter_order_by + add_column + materialize_column + drop_column + rename_column + clear_column
                       + modify_column + comment_column + delete_mask + heavy_update + add_stats + mod_stats + drop_stats + clear_stats
                       + mat_stats + add_idx + materialize_idx + clear_idx + drop_idx + column_remove_property + column_modify_setting
                       + column_remove_setting + table_modify_setting + table_remove_setting + add_projection + remove_projection
                       + materialize_projection + clear_projection + add_constraint + remove_constraint + detach_partition + 1))
            {
                const uint32_t nopt2 = rg.nextSmallNumber();
                PartitionExpr * pexpr = ati->mutable_detach_partition()->mutable_partition();

                if (table_has_partitions && nopt2 < 5)
                {
                    pexpr->set_partition_id(fc.tableGetRandomPartitionOrPart(false, true, dname, tname));
                }
                else if (table_has_partitions && nopt2 < 9)
                {
                    pexpr->set_part(fc.tableGetRandomPartitionOrPart(false, false, dname, tname));
                }
                else
                {
                    pexpr->set_all(true);
                }
            }
            else if (
                drop_partition
                && nopt
                    < (heavy_delete + alter_order_by + add_column + materialize_column + drop_column + rename_column + clear_column
                       + modify_column + comment_column + delete_mask + heavy_update + add_stats + mod_stats + drop_stats + clear_stats
                       + mat_stats + add_idx + materialize_idx + clear_idx + drop_idx + column_remove_property + column_modify_setting
                       + column_remove_setting + table_modify_setting + table_remove_setting + add_projection + remove_projection
                       + materialize_projection + clear_projection + add_constraint + remove_constraint + detach_partition + drop_partition
                       + 1))
            {
                const uint32_t nopt2 = rg.nextSmallNumber();
                PartitionExpr * pexpr = ati->mutable_drop_partition()->mutable_partition();

                if (table_has_partitions && nopt2 < 5)
                {
                    pexpr->set_partition_id(fc.tableGetRandomPartitionOrPart(false, true, dname, tname));
                }
                else if (table_has_partitions && nopt2 < 9)
                {
                    pexpr->set_part(fc.tableGetRandomPartitionOrPart(false, false, dname, tname));
                }
                else
                {
                    pexpr->set_all(true);
                }
            }
            else if (
                drop_detached_partition
                && nopt
                    < (heavy_delete + alter_order_by + add_column + materialize_column + drop_column + rename_column + clear_column
                       + modify_column + comment_column + delete_mask + heavy_update + add_stats + mod_stats + drop_stats + clear_stats
                       + mat_stats + add_idx + materialize_idx + clear_idx + drop_idx + column_remove_property + column_modify_setting
                       + column_remove_setting + table_modify_setting + table_remove_setting + add_projection + remove_projection
                       + materialize_projection + clear_projection + add_constraint + remove_constraint + detach_partition
                       + drop_detached_partition + 1))
            {
                const uint32_t nopt2 = rg.nextSmallNumber();
                PartitionExpr * pexpr = ati->mutable_drop_detached_partition()->mutable_partition();
                const bool table_has_detached_partitions = fc.tableHasPartitions(true, dname, tname);

                if (table_has_detached_partitions && nopt2 < 5)
                {
                    pexpr->set_partition_id(fc.tableGetRandomPartitionOrPart(true, true, dname, tname));
                }
                else if (table_has_detached_partitions && nopt2 < 9)
                {
                    pexpr->set_part(fc.tableGetRandomPartitionOrPart(true, false, dname, tname));
                }
                else
                {
                    pexpr->set_all(true);
                }
            }
            else if (
                forget_partition
                && nopt
                    < (heavy_delete + alter_order_by + add_column + materialize_column + drop_column + rename_column + clear_column
                       + modify_column + comment_column + delete_mask + heavy_update + add_stats + mod_stats + drop_stats + clear_stats
                       + mat_stats + add_idx + materialize_idx + clear_idx + drop_idx + column_remove_property + column_modify_setting
                       + column_remove_setting + table_modify_setting + table_remove_setting + add_projection + remove_projection
                       + materialize_projection + clear_projection + add_constraint + remove_constraint + detach_partition + drop_partition
                       + drop_detached_partition + forget_partition + 1))
            {
                PartitionExpr * pexpr = ati->mutable_forget_partition()->mutable_partition();

                pexpr->set_partition_id(fc.tableGetRandomPartitionOrPart(false, true, dname, tname));
            }
            else if (
                attach_partition
                && nopt
                    < (heavy_delete + alter_order_by + add_column + materialize_column + drop_column + rename_column + clear_column
                       + modify_column + comment_column + delete_mask + heavy_update + add_stats + mod_stats + drop_stats + clear_stats
                       + mat_stats + add_idx + materialize_idx + clear_idx + drop_idx + column_remove_property + column_modify_setting
                       + column_remove_setting + table_modify_setting + table_remove_setting + add_projection + remove_projection
                       + materialize_projection + clear_projection + add_constraint + remove_constraint + detach_partition + drop_partition
                       + drop_detached_partition + forget_partition + attach_partition + 1))
            {
                const uint32_t nopt2 = rg.nextSmallNumber();
                PartitionExpr * pexpr = ati->mutable_attach_partition()->mutable_partition();
                const bool table_has_detached_partitions = fc.tableHasPartitions(true, dname, tname);

                if (table_has_detached_partitions && nopt2 < 5)
                {
                    pexpr->set_partition_id(fc.tableGetRandomPartitionOrPart(true, true, dname, tname));
                }
                else if (table_has_detached_partitions && nopt2 < 9)
                {
                    pexpr->set_part(fc.tableGetRandomPartitionOrPart(true, false, dname, tname));
                }
                else
                {
                    pexpr->set_all(true);
                }
            }
            else if (
                move_partition_to
                && nopt
                    < (heavy_delete + alter_order_by + add_column + materialize_column + drop_column + rename_column + clear_column
                       + modify_column + comment_column + delete_mask + heavy_update + add_stats + mod_stats + drop_stats + clear_stats
                       + mat_stats + add_idx + materialize_idx + clear_idx + drop_idx + column_remove_property + column_modify_setting
                       + column_remove_setting + table_modify_setting + table_remove_setting + add_projection + remove_projection
                       + materialize_projection + clear_projection + add_constraint + remove_constraint + detach_partition + drop_partition
                       + drop_detached_partition + forget_partition + attach_partition + move_partition_to + 1))
            {
                AttachPartitionFrom * apf = ati->mutable_move_partition_to();
                PartitionExpr * pexpr = apf->mutable_single_partition()->mutable_partition();
                const SQLTable & t2 = rg.pickRandomly(filterCollection<SQLTable>(attached_tables));

                pexpr->set_partition_id(fc.tableGetRandomPartitionOrPart(false, true, dname, tname));
                t2.setName(apf->mutable_est(), false);
            }
            else if (
                clear_column_partition
                && nopt
                    < (heavy_delete + alter_order_by + add_column + materialize_column + drop_column + rename_column + clear_column
                       + modify_column + comment_column + delete_mask + heavy_update + add_stats + mod_stats + drop_stats + clear_stats
                       + mat_stats + add_idx + materialize_idx + clear_idx + drop_idx + column_remove_property + column_modify_setting
                       + column_remove_setting + table_modify_setting + table_remove_setting + add_projection + remove_projection
                       + materialize_projection + clear_projection + add_constraint + remove_constraint + detach_partition + drop_partition
                       + drop_detached_partition + forget_partition + attach_partition + move_partition_to + clear_column_partition + 1))
            {
                ClearColumnInPartition * ccip = ati->mutable_clear_column_partition();
                PartitionExpr * pexpr = ccip->mutable_single_partition()->mutable_partition();

                pexpr->set_partition_id(fc.tableGetRandomPartitionOrPart(false, true, dname, tname));
                flatTableColumnPath(flat_nested, t.cols, [](const SQLColumn &) { return true; });
                columnPathRef(rg.pickRandomly(this->entries), ccip->mutable_col());
                this->entries.clear();
            }
            else if (
                freeze_partition
                && nopt
                    < (heavy_delete + alter_order_by + add_column + materialize_column + drop_column + rename_column + clear_column
                       + modify_column + comment_column + delete_mask + heavy_update + add_stats + mod_stats + drop_stats + clear_stats
                       + mat_stats + add_idx + materialize_idx + clear_idx + drop_idx + column_remove_property + column_modify_setting
                       + column_remove_setting + table_modify_setting + table_remove_setting + add_projection + remove_projection
                       + materialize_projection + clear_projection + add_constraint + remove_constraint + detach_partition + drop_partition
                       + drop_detached_partition + forget_partition + attach_partition + move_partition_to + clear_column_partition
                       + freeze_partition + 1))
            {
                FreezePartition * fp = ati->mutable_freeze_partition();

                if (table_has_partitions && rg.nextSmallNumber() < 9)
                {
                    fp->mutable_single_partition()->mutable_partition()->set_partition_id(
                        fc.tableGetRandomPartitionOrPart(false, true, dname, tname));
                }
                fp->set_fname(t.freeze_counter++);
            }
            else if (
                unfreeze_partition
                && nopt
                    < (heavy_delete + alter_order_by + add_column + materialize_column + drop_column + rename_column + clear_column
                       + modify_column + comment_column + delete_mask + heavy_update + add_stats + mod_stats + drop_stats + clear_stats
                       + mat_stats + add_idx + materialize_idx + clear_idx + drop_idx + column_remove_property + column_modify_setting
                       + column_remove_setting + table_modify_setting + table_remove_setting + add_projection + remove_projection
                       + materialize_projection + clear_projection + add_constraint + remove_constraint + detach_partition + drop_partition
                       + drop_detached_partition + forget_partition + attach_partition + move_partition_to + clear_column_partition
                       + freeze_partition + unfreeze_partition + 1))
            {
                FreezePartition * fp = ati->mutable_unfreeze_partition();
                const uint32_t fname = rg.pickRandomly(t.frozen_partitions);
                const String & partition_id = t.frozen_partitions[fname];

                if (!partition_id.empty())
                {
                    fp->mutable_single_partition()->mutable_partition()->set_partition_id(partition_id);
                }
                fp->set_fname(fname);
            }
            else if (
                clear_index_partition
                && nopt
                    < (heavy_delete + alter_order_by + add_column + materialize_column + drop_column + rename_column + clear_column
                       + modify_column + comment_column + delete_mask + heavy_update + add_stats + mod_stats + drop_stats + clear_stats
                       + mat_stats + add_idx + materialize_idx + clear_idx + drop_idx + column_remove_property + column_modify_setting
                       + column_remove_setting + table_modify_setting + table_remove_setting + add_projection + remove_projection
                       + materialize_projection + clear_projection + add_constraint + remove_constraint + detach_partition + drop_partition
                       + drop_detached_partition + forget_partition + attach_partition + move_partition_to + clear_column_partition
                       + freeze_partition + unfreeze_partition + clear_index_partition + 1))
            {
                ClearIndexInPartition * ccip = ati->mutable_clear_index_partition();
                PartitionExpr * pexpr = ccip->mutable_single_partition()->mutable_partition();

                pexpr->set_partition_id(fc.tableGetRandomPartitionOrPart(false, true, dname, tname));
                ccip->mutable_idx()->set_index("i" + std::to_string(rg.pickRandomly(t.idxs)));
            }
            else if (
                move_partition
                && nopt
                    < (heavy_delete + alter_order_by + add_column + materialize_column + drop_column + rename_column + clear_column
                       + modify_column + comment_column + delete_mask + heavy_update + add_stats + mod_stats + drop_stats + clear_stats
                       + mat_stats + add_idx + materialize_idx + clear_idx + drop_idx + column_remove_property + column_modify_setting
                       + column_remove_setting + table_modify_setting + table_remove_setting + add_projection + remove_projection
                       + materialize_projection + clear_projection + add_constraint + remove_constraint + detach_partition + drop_partition
                       + drop_detached_partition + forget_partition + attach_partition + move_partition_to + clear_column_partition
                       + freeze_partition + unfreeze_partition + clear_index_partition + move_partition + 1))
            {
                MovePartition * mp = ati->mutable_move_partition();
                PartitionExpr * pexpr = mp->mutable_single_partition()->mutable_partition();

                pexpr->set_partition_id(fc.tableGetRandomPartitionOrPart(false, true, dname, tname));
                generateStorage(rg, mp->mutable_storage());
            }
            else if (
                modify_ttl
                && nopt
                    < (heavy_delete + alter_order_by + add_column + materialize_column + drop_column + rename_column + clear_column
                       + modify_column + comment_column + delete_mask + heavy_update + add_stats + mod_stats + drop_stats + clear_stats
                       + mat_stats + add_idx + materialize_idx + clear_idx + drop_idx + column_remove_property + column_modify_setting
                       + column_remove_setting + table_modify_setting + table_remove_setting + add_projection + remove_projection
                       + materialize_projection + clear_projection + add_constraint + remove_constraint + detach_partition + drop_partition
                       + drop_detached_partition + forget_partition + attach_partition + move_partition_to + clear_column_partition
                       + freeze_partition + unfreeze_partition + clear_index_partition + move_partition + modify_ttl + 1))
            {
                flatTableColumnPath(0, t.cols, [](const SQLColumn & c) { return c.tp->getTypeClass() != SQLTypeClass::NESTED; });
                generateNextTTL(rg, t, nullptr, ati->mutable_modify_ttl());
                this->entries.clear();
            }
            else if (
                remove_ttl
                && nopt
                    < (heavy_delete + alter_order_by + add_column + materialize_column + drop_column + rename_column + clear_column
                       + modify_column + comment_column + delete_mask + heavy_update + add_stats + mod_stats + drop_stats + clear_stats
                       + mat_stats + add_idx + materialize_idx + clear_idx + drop_idx + column_remove_property + column_modify_setting
                       + column_remove_setting + table_modify_setting + table_remove_setting + add_projection + remove_projection
                       + materialize_projection + clear_projection + add_constraint + remove_constraint + detach_partition + drop_partition
                       + drop_detached_partition + forget_partition + attach_partition + move_partition_to + clear_column_partition
                       + freeze_partition + unfreeze_partition + clear_index_partition + move_partition + modify_ttl + remove_ttl + 1))
            {
                ati->set_remove_ttl(true);
            }
            else if (
                comment_table
                && nopt
                    < (heavy_delete + alter_order_by + add_column + materialize_column + drop_column + rename_column + clear_column
                       + modify_column + comment_column + delete_mask + heavy_update + add_stats + mod_stats + drop_stats + clear_stats
                       + mat_stats + add_idx + materialize_idx + clear_idx + drop_idx + column_remove_property + column_modify_setting
                       + column_remove_setting + table_modify_setting + table_remove_setting + add_projection + remove_projection
                       + materialize_projection + clear_projection + add_constraint + remove_constraint + detach_partition + drop_partition
                       + drop_detached_partition + forget_partition + attach_partition + move_partition_to + clear_column_partition
                       + freeze_partition + unfreeze_partition + clear_index_partition + move_partition + modify_ttl + remove_ttl
                       + comment_table + 1))
            {
                ati->set_comment(rg.nextString("'", true, rg.nextRandomUInt32() % 1009));
            }
            else
            {
                chassert(0);
            }
        }
    }
    else
    {
        chassert(0);
    }
    if (cluster.has_value())
    {
        at->mutable_cluster()->set_cluster(cluster.value());
    }
    if (rg.nextSmallNumber() < 3)
    {
        generateSettingValues(rg, serverSettings, at->mutable_setting_values());
    }
}

void StatementGenerator::generateAttach(RandomGenerator & rg, Attach * att)
{
    SQLObjectName * sot = att->mutable_object();
    const uint32_t attach_table = 10 * static_cast<uint32_t>(collectionHas<SQLTable>(detached_tables));
    const uint32_t attach_view = 10 * static_cast<uint32_t>(collectionHas<SQLView>(detached_views));
    const uint32_t attach_dictionary = 10 * static_cast<uint32_t>(collectionHas<SQLDictionary>(detached_dictionaries));
    const uint32_t attach_database = 2 * static_cast<uint32_t>(collectionHas<std::shared_ptr<SQLDatabase>>(detached_databases));
    const uint32_t prob_space = attach_table + attach_view + attach_dictionary + attach_database;
    std::uniform_int_distribution<uint32_t> next_dist(1, prob_space);
    const uint32_t nopt = next_dist(rg.generator);
    std::optional<String> cluster;

    if (attach_table && nopt < (attach_table + 1))
    {
        const SQLTable & t = rg.pickRandomly(filterCollection<SQLTable>(detached_tables));

        cluster = t.getCluster();
        att->set_sobject(SQLObject::TABLE);
        t.setName(sot->mutable_est(), false);
    }
    else if (attach_view && nopt < (attach_table + attach_view + 1))
    {
        const SQLView & v = rg.pickRandomly(filterCollection<SQLView>(detached_views));

        cluster = v.getCluster();
        att->set_sobject(SQLObject::TABLE);
        v.setName(sot->mutable_est(), false);
    }
    else if (attach_dictionary && nopt < (attach_table + attach_view + attach_dictionary + 1))
    {
        const SQLDictionary & d = rg.pickRandomly(filterCollection<SQLDictionary>(detached_dictionaries));

        cluster = d.getCluster();
        att->set_sobject(SQLObject::DICTIONARY);
        d.setName(sot->mutable_est(), false);
    }
    else if (attach_database && nopt < (attach_table + attach_view + attach_dictionary + attach_database + 1))
    {
        const std::shared_ptr<SQLDatabase> & d = rg.pickRandomly(filterCollection<std::shared_ptr<SQLDatabase>>(detached_databases));

        cluster = d->getCluster();
        att->set_sobject(SQLObject::DATABASE);
        d->setName(sot->mutable_database());
    }
    else
    {
        chassert(0);
    }
    if (cluster.has_value())
    {
        att->mutable_cluster()->set_cluster(cluster.value());
    }
    if (att->sobject() != SQLObject::DATABASE && rg.nextSmallNumber() < 3)
    {
        att->set_as_replicated(rg.nextBool());
    }
    if (rg.nextSmallNumber() < 3)
    {
        generateSettingValues(rg, serverSettings, att->mutable_setting_values());
    }
}

void StatementGenerator::generateDetach(RandomGenerator & rg, Detach * det)
{
    SQLObjectName * sot = det->mutable_object();
    const uint32_t detach_table = 10 * static_cast<uint32_t>(collectionCount<SQLTable>(attached_tables) > 3);
    const uint32_t detach_view = 10 * static_cast<uint32_t>(collectionCount<SQLView>(attached_views) > 3);
    const uint32_t detach_dictionary = 10 * static_cast<uint32_t>(collectionCount<SQLDictionary>(attached_dictionaries) > 3);
    const uint32_t detach_database = 2 * static_cast<uint32_t>(collectionCount<std::shared_ptr<SQLDatabase>>(attached_databases) > 3);
    const uint32_t prob_space = detach_table + detach_view + detach_dictionary + detach_database;
    std::uniform_int_distribution<uint32_t> next_dist(1, prob_space);
    const uint32_t nopt = next_dist(rg.generator);
    std::optional<String> cluster;

    if (detach_table && nopt < (detach_table + 1))
    {
        const SQLTable & t = rg.pickRandomly(filterCollection<SQLTable>(attached_tables));

        cluster = t.getCluster();
        det->set_sobject(SQLObject::TABLE);
        t.setName(sot->mutable_est(), false);
    }
    else if (detach_view && nopt < (detach_table + detach_view + 1))
    {
        const SQLView & v = rg.pickRandomly(filterCollection<SQLView>(attached_views));

        cluster = v.getCluster();
        det->set_sobject(SQLObject::TABLE);
        v.setName(sot->mutable_est(), false);
    }
    else if (detach_dictionary && nopt < (detach_table + detach_view + detach_dictionary + 1))
    {
        const SQLDictionary & d = rg.pickRandomly(filterCollection<SQLDictionary>(attached_dictionaries));

        cluster = d.getCluster();
        det->set_sobject(SQLObject::DICTIONARY);
        d.setName(sot->mutable_est(), false);
    }
    else if (detach_database && nopt < (detach_table + detach_view + detach_dictionary + detach_database + 1))
    {
        const std::shared_ptr<SQLDatabase> & d = rg.pickRandomly(filterCollection<std::shared_ptr<SQLDatabase>>(attached_databases));

        cluster = d->getCluster();
        det->set_sobject(SQLObject::DATABASE);
        d->setName(sot->mutable_database());
    }
    else
    {
        chassert(0);
    }
    if (cluster.has_value())
    {
        det->mutable_cluster()->set_cluster(cluster.value());
    }
    det->set_permanently(!detach_database && rg.nextSmallNumber() < 4);
    det->set_sync(rg.nextSmallNumber() < 4);
    if (rg.nextSmallNumber() < 3)
    {
        generateSettingValues(rg, serverSettings, det->mutable_setting_values());
    }
}

static const auto has_merge_tree_func = [](const SQLTable & t) { return t.isAttached() && t.isMergeTreeFamily(); };

static const auto has_refreshable_view_func = [](const SQLView & v) { return v.isAttached() && v.is_refreshable; };

void StatementGenerator::generateNextSystemStatement(RandomGenerator & rg, SystemCommand * sc)
{
    const uint32_t has_merge_tree = static_cast<uint32_t>(collectionHas<SQLTable>(has_merge_tree_func));
    const uint32_t has_refreshable_view = static_cast<uint32_t>(collectionHas<SQLView>(has_refreshable_view_func));
    const uint32_t reload_embedded_dictionaries = 1;
    const uint32_t reload_dictionaries = 3;
    const uint32_t reload_models = 3;
    const uint32_t reload_functions = 3;
    const uint32_t reload_function = 8 * static_cast<uint32_t>(!functions.empty());
    const uint32_t reload_asynchronous_metrics = 3;
    const uint32_t drop_dns_cache = 3;
    const uint32_t drop_mark_cache = 3;
    const uint32_t drop_uncompressed_cache = 9;
    const uint32_t drop_compiled_expression_cache = 3;
    const uint32_t drop_query_cache = 3;
    const uint32_t drop_format_schema_cache = 3;
    const uint32_t flush_logs = 3;
    const uint32_t reload_config = 3;
    const uint32_t reload_users = 3;
    /// For merge trees
    const uint32_t stop_merges = 0 * has_merge_tree;
    const uint32_t start_merges = 0 * has_merge_tree;
    const uint32_t stop_ttl_merges = 8 * has_merge_tree;
    const uint32_t start_ttl_merges = 8 * has_merge_tree;
    const uint32_t stop_moves = 8 * has_merge_tree;
    const uint32_t start_moves = 8 * has_merge_tree;
    const uint32_t wait_loading_parts = 8 * has_merge_tree;
    /// For replicated merge trees
    const uint32_t stop_fetches = 8 * has_merge_tree;
    const uint32_t start_fetches = 8 * has_merge_tree;
    const uint32_t stop_replicated_sends = 8 * has_merge_tree;
    const uint32_t start_replicated_sends = 8 * has_merge_tree;
    const uint32_t stop_replication_queues = 0 * has_merge_tree;
    const uint32_t start_replication_queues = 0 * has_merge_tree;
    const uint32_t stop_pulling_replication_log = 0 * has_merge_tree;
    const uint32_t start_pulling_replication_log = 0 * has_merge_tree;
    const uint32_t sync_replica = 8 * has_merge_tree;
    const uint32_t sync_replicated_database = 8 * static_cast<uint32_t>(collectionHas<std::shared_ptr<SQLDatabase>>(attached_databases));
    const uint32_t restart_replica = 8 * has_merge_tree;
    const uint32_t restore_replica = 8 * has_merge_tree;
    const uint32_t restart_replicas = 3;
    const uint32_t drop_filesystem_cache = 3;
    const uint32_t sync_file_cache = 1;
    /// For merge trees
    const uint32_t load_pks = 3;
    const uint32_t load_pk = 8 * has_merge_tree;
    const uint32_t unload_pks = 3;
    const uint32_t unload_pk = 8 * has_merge_tree;
    /// for refreshable views
    const uint32_t refresh_views = 3;
    const uint32_t refresh_view = 8 * has_refreshable_view;
    const uint32_t stop_views = 3;
    const uint32_t stop_view = 8 * has_refreshable_view;
    const uint32_t start_views = 3;
    const uint32_t start_view = 8 * has_refreshable_view;
    const uint32_t cancel_view = 8 * has_refreshable_view;
    const uint32_t wait_view = 8 * has_refreshable_view;
    const uint32_t prewarm_cache = 8 * has_merge_tree;
    const uint32_t prewarm_primary_index_cache = 8 * has_merge_tree;
    const uint32_t drop_connections_cache = 3;
    const uint32_t drop_primary_index_cache = 3;
    const uint32_t drop_index_mark_cache = 3;
    const uint32_t drop_index_uncompressed_cache = 3;
    const uint32_t drop_mmap_cache = 3;
    const uint32_t drop_page_cache = 3;
    const uint32_t drop_schema_cache = 3;
    const uint32_t drop_s3_client_cache = 3;
    const uint32_t flush_async_insert_queue = 3;
    const uint32_t sync_filesystem_cache = 3;
    const uint32_t drop_cache = 3;
    const uint32_t drop_skip_index_cache = 3;
    const uint32_t prob_space = reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + reload_function
        + reload_asynchronous_metrics + drop_dns_cache + drop_mark_cache + drop_uncompressed_cache + drop_compiled_expression_cache
        + drop_query_cache + drop_format_schema_cache + flush_logs + reload_config + reload_users + stop_merges + start_merges
        + stop_ttl_merges + start_ttl_merges + stop_moves + start_moves + wait_loading_parts + stop_fetches + start_fetches
        + stop_replicated_sends + start_replicated_sends + stop_replication_queues + start_replication_queues + stop_pulling_replication_log
        + start_pulling_replication_log + sync_replica + sync_replicated_database + restart_replica + restore_replica + restart_replicas
        + drop_filesystem_cache + sync_file_cache + load_pks + load_pk + unload_pks + unload_pk + refresh_views + refresh_view + stop_views
        + stop_view + start_views + start_view + cancel_view + wait_view + prewarm_cache + prewarm_primary_index_cache
        + drop_connections_cache + drop_primary_index_cache + drop_index_mark_cache + drop_index_uncompressed_cache + drop_mmap_cache
        + drop_page_cache + drop_schema_cache + drop_s3_client_cache + flush_async_insert_queue + sync_filesystem_cache + drop_cache
        + drop_skip_index_cache;
    std::uniform_int_distribution<uint32_t> next_dist(1, prob_space);
    const uint32_t nopt = next_dist(rg.generator);

    if (reload_embedded_dictionaries && nopt < (reload_embedded_dictionaries + 1))
    {
        sc->set_reload_embedded_dictionaries(true);
    }
    else if (reload_dictionaries && nopt < (reload_embedded_dictionaries + reload_dictionaries + 1))
    {
        sc->set_reload_dictionaries(true);
    }
    else if (reload_models && nopt < (reload_embedded_dictionaries + reload_dictionaries + reload_models + 1))
    {
        sc->set_reload_models(true);
    }
    else if (reload_functions && nopt < (reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + 1))
    {
        sc->set_reload_functions(true);
    }
    else if (
        reload_function
        && nopt < (reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + reload_function + 1))
    {
        const SQLFunction & f = rg.pickValueRandomlyFromMap(this->functions);

        f.setName(sc->mutable_reload_function());
    }
    else if (
        reload_asynchronous_metrics
        && nopt
            < (reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + reload_function
               + reload_asynchronous_metrics + 1))
    {
        sc->set_reload_asynchronous_metrics(true);
    }
    else if (
        drop_dns_cache
        && nopt
            < (reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + reload_function
               + reload_asynchronous_metrics + drop_dns_cache + 1))
    {
        sc->set_drop_dns_cache(true);
    }
    else if (
        drop_mark_cache
        && nopt
            < (reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + reload_function
               + reload_asynchronous_metrics + drop_dns_cache + drop_mark_cache + 1))
    {
        sc->set_drop_mark_cache(true);
    }
    else if (
        drop_uncompressed_cache
        && nopt
            < (reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + reload_function
               + reload_asynchronous_metrics + drop_dns_cache + drop_mark_cache + drop_uncompressed_cache + 1))
    {
        sc->set_drop_uncompressed_cache(true);
    }
    else if (
        drop_compiled_expression_cache
        && nopt
            < (reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + reload_function
               + reload_asynchronous_metrics + drop_dns_cache + drop_mark_cache + drop_uncompressed_cache + drop_compiled_expression_cache
               + 1))
    {
        sc->set_drop_compiled_expression_cache(true);
    }
    else if (
        drop_query_cache
        && nopt
            < (reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + reload_function
               + reload_asynchronous_metrics + drop_dns_cache + drop_mark_cache + drop_uncompressed_cache + drop_compiled_expression_cache
               + drop_query_cache + 1))
    {
        sc->set_drop_query_cache(true);
    }
    else if (
        drop_format_schema_cache
        && nopt
            < (reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + reload_function
               + reload_asynchronous_metrics + drop_dns_cache + drop_mark_cache + drop_uncompressed_cache + drop_compiled_expression_cache
               + drop_query_cache + drop_format_schema_cache + 1))
    {
        sc->set_drop_format_schema_cache(rg.nextBool());
    }
    else if (
        flush_logs
        && nopt
            < (reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + reload_function
               + reload_asynchronous_metrics + drop_dns_cache + drop_mark_cache + drop_uncompressed_cache + drop_compiled_expression_cache
               + drop_query_cache + drop_format_schema_cache + flush_logs + 1))
    {
        sc->set_flush_logs(true);
    }
    else if (
        reload_config
        && nopt
            < (reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + reload_function
               + reload_asynchronous_metrics + drop_dns_cache + drop_mark_cache + drop_uncompressed_cache + drop_compiled_expression_cache
               + drop_query_cache + drop_format_schema_cache + flush_logs + reload_config + 1))
    {
        sc->set_reload_config(true);
    }
    else if (
        reload_users
        && nopt
            < (reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + reload_function
               + reload_asynchronous_metrics + drop_dns_cache + drop_mark_cache + drop_uncompressed_cache + drop_compiled_expression_cache
               + drop_query_cache + drop_format_schema_cache + flush_logs + reload_config + reload_users + 1))
    {
        sc->set_reload_users(true);
    }
    else if (
        stop_merges
        && nopt
            < (reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + reload_function
               + reload_asynchronous_metrics + drop_dns_cache + drop_mark_cache + drop_uncompressed_cache + drop_compiled_expression_cache
               + drop_query_cache + drop_format_schema_cache + flush_logs + reload_config + reload_users + stop_merges + 1))
    {
        setTableSystemStatement<SQLTable>(rg, has_merge_tree_func, sc->mutable_stop_merges());
    }
    else if (
        start_merges
        && nopt
            < (reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + reload_function
               + reload_asynchronous_metrics + drop_dns_cache + drop_mark_cache + drop_uncompressed_cache + drop_compiled_expression_cache
               + drop_query_cache + drop_format_schema_cache + flush_logs + reload_config + reload_users + stop_merges + start_merges + 1))
    {
        setTableSystemStatement<SQLTable>(rg, has_merge_tree_func, sc->mutable_start_merges());
    }
    else if (
        stop_ttl_merges
        && nopt
            < (reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + reload_function
               + reload_asynchronous_metrics + drop_dns_cache + drop_mark_cache + drop_uncompressed_cache + drop_compiled_expression_cache
               + drop_query_cache + drop_format_schema_cache + flush_logs + reload_config + reload_users + stop_merges + start_merges
               + stop_ttl_merges + 1))
    {
        setTableSystemStatement<SQLTable>(rg, has_merge_tree_func, sc->mutable_stop_ttl_merges());
    }
    else if (
        start_ttl_merges
        && nopt
            < (reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + reload_function
               + reload_asynchronous_metrics + drop_dns_cache + drop_mark_cache + drop_uncompressed_cache + drop_compiled_expression_cache
               + drop_query_cache + drop_format_schema_cache + flush_logs + reload_config + reload_users + stop_merges + start_merges
               + stop_ttl_merges + start_ttl_merges + 1))
    {
        setTableSystemStatement<SQLTable>(rg, has_merge_tree_func, sc->mutable_start_ttl_merges());
    }
    else if (
        stop_moves
        && nopt
            < (reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + reload_function
               + reload_asynchronous_metrics + drop_dns_cache + drop_mark_cache + drop_uncompressed_cache + drop_compiled_expression_cache
               + drop_query_cache + drop_format_schema_cache + flush_logs + reload_config + reload_users + stop_merges + start_merges
               + stop_ttl_merges + start_ttl_merges + stop_moves + 1))
    {
        setTableSystemStatement<SQLTable>(rg, has_merge_tree_func, sc->mutable_stop_moves());
    }
    else if (
        start_moves
        && nopt
            < (reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + reload_function
               + reload_asynchronous_metrics + drop_dns_cache + drop_mark_cache + drop_uncompressed_cache + drop_compiled_expression_cache
               + drop_query_cache + drop_format_schema_cache + flush_logs + reload_config + reload_users + stop_merges + start_merges
               + stop_ttl_merges + start_ttl_merges + stop_moves + start_moves + 1))
    {
        setTableSystemStatement<SQLTable>(rg, has_merge_tree_func, sc->mutable_start_moves());
    }
    else if (
        wait_loading_parts
        && nopt
            < (reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + reload_function
               + reload_asynchronous_metrics + drop_dns_cache + drop_mark_cache + drop_uncompressed_cache + drop_compiled_expression_cache
               + drop_query_cache + drop_format_schema_cache + flush_logs + reload_config + reload_users + stop_merges + start_merges
               + stop_ttl_merges + start_ttl_merges + stop_moves + start_moves + wait_loading_parts + 1))
    {
        setTableSystemStatement<SQLTable>(rg, has_merge_tree_func, sc->mutable_wait_loading_parts());
    }
    else if (
        stop_fetches
        && nopt
            < (reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + reload_function
               + reload_asynchronous_metrics + drop_dns_cache + drop_mark_cache + drop_uncompressed_cache + drop_compiled_expression_cache
               + drop_query_cache + drop_format_schema_cache + flush_logs + reload_config + reload_users + stop_merges + start_merges
               + stop_ttl_merges + start_ttl_merges + stop_moves + start_moves + wait_loading_parts + stop_fetches + 1))
    {
        setTableSystemStatement<SQLTable>(rg, has_merge_tree_func, sc->mutable_stop_fetches());
    }
    else if (
        start_fetches
        && nopt
            < (reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + reload_function
               + reload_asynchronous_metrics + drop_dns_cache + drop_mark_cache + drop_uncompressed_cache + drop_compiled_expression_cache
               + drop_query_cache + drop_format_schema_cache + flush_logs + reload_config + reload_users + stop_merges + start_merges
               + stop_ttl_merges + start_ttl_merges + stop_moves + start_moves + wait_loading_parts + stop_fetches + start_fetches + 1))
    {
        setTableSystemStatement<SQLTable>(rg, has_merge_tree_func, sc->mutable_start_fetches());
    }
    else if (
        stop_replicated_sends
        && nopt
            < (reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + reload_function
               + reload_asynchronous_metrics + drop_dns_cache + drop_mark_cache + drop_uncompressed_cache + drop_compiled_expression_cache
               + drop_query_cache + drop_format_schema_cache + flush_logs + reload_config + reload_users + stop_merges + start_merges
               + stop_ttl_merges + start_ttl_merges + stop_moves + start_moves + wait_loading_parts + stop_fetches + start_fetches
               + stop_replicated_sends + 1))
    {
        setTableSystemStatement<SQLTable>(rg, has_merge_tree_func, sc->mutable_stop_replicated_sends());
    }
    else if (
        start_replicated_sends
        && nopt
            < (reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + reload_function
               + reload_asynchronous_metrics + drop_dns_cache + drop_mark_cache + drop_uncompressed_cache + drop_compiled_expression_cache
               + drop_query_cache + drop_format_schema_cache + flush_logs + reload_config + reload_users + stop_merges + start_merges
               + stop_ttl_merges + start_ttl_merges + stop_moves + start_moves + wait_loading_parts + stop_fetches + start_fetches
               + stop_replicated_sends + start_replicated_sends + 1))
    {
        setTableSystemStatement<SQLTable>(rg, has_merge_tree_func, sc->mutable_start_replicated_sends());
    }
    else if (
        stop_replication_queues
        && nopt
            < (reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + reload_function
               + reload_asynchronous_metrics + drop_dns_cache + drop_mark_cache + drop_uncompressed_cache + drop_compiled_expression_cache
               + drop_query_cache + drop_format_schema_cache + flush_logs + reload_config + reload_users + stop_merges + start_merges
               + stop_ttl_merges + start_ttl_merges + stop_moves + start_moves + wait_loading_parts + stop_fetches + start_fetches
               + stop_replicated_sends + start_replicated_sends + stop_replication_queues + 1))
    {
        setTableSystemStatement<SQLTable>(rg, has_merge_tree_func, sc->mutable_stop_replication_queues());
    }
    else if (
        start_replication_queues
        && nopt
            < (reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + reload_function
               + reload_asynchronous_metrics + drop_dns_cache + drop_mark_cache + drop_uncompressed_cache + drop_compiled_expression_cache
               + drop_query_cache + drop_format_schema_cache + flush_logs + reload_config + reload_users + stop_merges + start_merges
               + stop_ttl_merges + start_ttl_merges + stop_moves + start_moves + wait_loading_parts + stop_fetches + start_fetches
               + stop_replicated_sends + start_replicated_sends + stop_replication_queues + start_replication_queues + 1))
    {
        setTableSystemStatement<SQLTable>(rg, has_merge_tree_func, sc->mutable_start_replication_queues());
    }
    else if (
        stop_pulling_replication_log
        && nopt
            < (reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + reload_function
               + reload_asynchronous_metrics + drop_dns_cache + drop_mark_cache + drop_uncompressed_cache + drop_compiled_expression_cache
               + drop_query_cache + drop_format_schema_cache + flush_logs + reload_config + reload_users + stop_merges + start_merges
               + stop_ttl_merges + start_ttl_merges + stop_moves + start_moves + wait_loading_parts + stop_fetches + start_fetches
               + stop_replicated_sends + start_replicated_sends + stop_replication_queues + start_replication_queues
               + stop_pulling_replication_log + 1))
    {
        setTableSystemStatement<SQLTable>(rg, has_merge_tree_func, sc->mutable_stop_pulling_replication_log());
    }
    else if (
        start_pulling_replication_log
        && nopt
            < (reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + reload_function
               + reload_asynchronous_metrics + drop_dns_cache + drop_mark_cache + drop_uncompressed_cache + drop_compiled_expression_cache
               + drop_query_cache + drop_format_schema_cache + flush_logs + reload_config + reload_users + stop_merges + start_merges
               + stop_ttl_merges + start_ttl_merges + stop_moves + start_moves + wait_loading_parts + stop_fetches + start_fetches
               + stop_replicated_sends + start_replicated_sends + stop_replication_queues + start_replication_queues
               + stop_pulling_replication_log + start_pulling_replication_log + 1))
    {
        setTableSystemStatement<SQLTable>(rg, has_merge_tree_func, sc->mutable_start_pulling_replication_log());
    }
    else if (
        sync_replica
        && nopt
            < (reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + reload_function
               + reload_asynchronous_metrics + drop_dns_cache + drop_mark_cache + drop_uncompressed_cache + drop_compiled_expression_cache
               + drop_query_cache + drop_format_schema_cache + flush_logs + reload_config + reload_users + stop_merges + start_merges
               + stop_ttl_merges + start_ttl_merges + stop_moves + start_moves + wait_loading_parts + stop_fetches + start_fetches
               + stop_replicated_sends + start_replicated_sends + stop_replication_queues + start_replication_queues
               + stop_pulling_replication_log + start_pulling_replication_log + sync_replica + 1))
    {
        SyncReplica * srep = sc->mutable_sync_replica();

        srep->set_policy(
            static_cast<SyncReplica_SyncPolicy>((rg.nextRandomUInt32() % static_cast<uint32_t>(SyncReplica::SyncPolicy_MAX)) + 1));
        setTableSystemStatement<SQLTable>(rg, has_merge_tree_func, srep->mutable_est());
    }
    else if (
        sync_replicated_database
        && nopt
            < (reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + reload_function
               + reload_asynchronous_metrics + drop_dns_cache + drop_mark_cache + drop_uncompressed_cache + drop_compiled_expression_cache
               + drop_query_cache + drop_format_schema_cache + flush_logs + reload_config + reload_users + stop_merges + start_merges
               + stop_ttl_merges + start_ttl_merges + stop_moves + start_moves + wait_loading_parts + stop_fetches + start_fetches
               + stop_replicated_sends + start_replicated_sends + stop_replication_queues + start_replication_queues
               + stop_pulling_replication_log + start_pulling_replication_log + sync_replica + sync_replicated_database + 1))
    {
        const std::shared_ptr<SQLDatabase> & d = rg.pickRandomly(filterCollection<std::shared_ptr<SQLDatabase>>(attached_databases));

        d->setName(sc->mutable_sync_replicated_database());
    }
    else if (
        restart_replica
        && nopt
            < (reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + reload_function
               + reload_asynchronous_metrics + drop_dns_cache + drop_mark_cache + drop_uncompressed_cache + drop_compiled_expression_cache
               + drop_query_cache + drop_format_schema_cache + flush_logs + reload_config + reload_users + stop_merges + start_merges
               + stop_ttl_merges + start_ttl_merges + stop_moves + start_moves + wait_loading_parts + stop_fetches + start_fetches
               + stop_replicated_sends + start_replicated_sends + stop_replication_queues + start_replication_queues
               + stop_pulling_replication_log + start_pulling_replication_log + sync_replica + sync_replicated_database + restart_replica
               + 1))
    {
        setTableSystemStatement<SQLTable>(rg, has_merge_tree_func, sc->mutable_restart_replica());
    }
    else if (
        restore_replica
        && nopt
            < (reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + reload_function
               + reload_asynchronous_metrics + drop_dns_cache + drop_mark_cache + drop_uncompressed_cache + drop_compiled_expression_cache
               + drop_query_cache + drop_format_schema_cache + flush_logs + reload_config + reload_users + stop_merges + start_merges
               + stop_ttl_merges + start_ttl_merges + stop_moves + start_moves + wait_loading_parts + stop_fetches + start_fetches
               + stop_replicated_sends + start_replicated_sends + stop_replication_queues + start_replication_queues
               + stop_pulling_replication_log + start_pulling_replication_log + sync_replica + sync_replicated_database + restart_replica
               + restore_replica + 1))
    {
        setTableSystemStatement<SQLTable>(rg, has_merge_tree_func, sc->mutable_restore_replica());
    }
    else if (
        restart_replicas
        && nopt
            < (reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + reload_function
               + reload_asynchronous_metrics + drop_dns_cache + drop_mark_cache + drop_uncompressed_cache + drop_compiled_expression_cache
               + drop_query_cache + drop_format_schema_cache + flush_logs + reload_config + reload_users + stop_merges + start_merges
               + stop_ttl_merges + start_ttl_merges + stop_moves + start_moves + wait_loading_parts + stop_fetches + start_fetches
               + stop_replicated_sends + start_replicated_sends + stop_replication_queues + start_replication_queues
               + stop_pulling_replication_log + start_pulling_replication_log + sync_replica + sync_replicated_database + restart_replica
               + restore_replica + restart_replicas + 1))
    {
        sc->set_restart_replicas(true);
    }
    else if (
        drop_filesystem_cache
        && nopt
            < (reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + reload_function
               + reload_asynchronous_metrics + drop_dns_cache + drop_mark_cache + drop_uncompressed_cache + drop_compiled_expression_cache
               + drop_query_cache + drop_format_schema_cache + flush_logs + reload_config + reload_users + stop_merges + start_merges
               + stop_ttl_merges + start_ttl_merges + stop_moves + start_moves + wait_loading_parts + stop_fetches + start_fetches
               + stop_replicated_sends + start_replicated_sends + stop_replication_queues + start_replication_queues
               + stop_pulling_replication_log + start_pulling_replication_log + sync_replica + sync_replicated_database + restart_replica
               + restore_replica + restart_replicas + drop_filesystem_cache + 1))
    {
        sc->set_drop_filesystem_cache(true);
    }
    else if (
        sync_file_cache
        && nopt
            < (reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + reload_function
               + reload_asynchronous_metrics + drop_dns_cache + drop_mark_cache + drop_uncompressed_cache + drop_compiled_expression_cache
               + drop_query_cache + drop_format_schema_cache + flush_logs + reload_config + reload_users + stop_merges + start_merges
               + stop_ttl_merges + start_ttl_merges + stop_moves + start_moves + wait_loading_parts + stop_fetches + start_fetches
               + stop_replicated_sends + start_replicated_sends + stop_replication_queues + start_replication_queues
               + stop_pulling_replication_log + start_pulling_replication_log + sync_replica + sync_replicated_database + restart_replica
               + restore_replica + restart_replicas + sync_file_cache + drop_filesystem_cache + 1))
    {
        sc->set_sync_file_cache(true);
    }
    else if (
        load_pks
        && nopt
            < (reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + reload_function
               + reload_asynchronous_metrics + drop_dns_cache + drop_mark_cache + drop_uncompressed_cache + drop_compiled_expression_cache
               + drop_query_cache + drop_format_schema_cache + flush_logs + reload_config + reload_users + stop_merges + start_merges
               + stop_ttl_merges + start_ttl_merges + stop_moves + start_moves + wait_loading_parts + stop_fetches + start_fetches
               + stop_replicated_sends + start_replicated_sends + stop_replication_queues + start_replication_queues
               + stop_pulling_replication_log + start_pulling_replication_log + sync_replica + sync_replicated_database + restart_replica
               + restore_replica + restart_replicas + sync_file_cache + drop_filesystem_cache + load_pks + 1))
    {
        sc->set_load_pks(true);
    }
    else if (
        load_pk
        && nopt
            < (reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + reload_function
               + reload_asynchronous_metrics + drop_dns_cache + drop_mark_cache + drop_uncompressed_cache + drop_compiled_expression_cache
               + drop_query_cache + drop_format_schema_cache + flush_logs + reload_config + reload_users + stop_merges + start_merges
               + stop_ttl_merges + start_ttl_merges + stop_moves + start_moves + wait_loading_parts + stop_fetches + start_fetches
               + stop_replicated_sends + start_replicated_sends + stop_replication_queues + start_replication_queues
               + stop_pulling_replication_log + start_pulling_replication_log + sync_replica + sync_replicated_database + restart_replica
               + restore_replica + restart_replicas + sync_file_cache + drop_filesystem_cache + load_pks + load_pk + 1))
    {
        setTableSystemStatement<SQLTable>(rg, has_merge_tree_func, sc->mutable_load_pk());
    }
    else if (
        unload_pks
        && nopt
            < (reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + reload_function
               + reload_asynchronous_metrics + drop_dns_cache + drop_mark_cache + drop_uncompressed_cache + drop_compiled_expression_cache
               + drop_query_cache + drop_format_schema_cache + flush_logs + reload_config + reload_users + stop_merges + start_merges
               + stop_ttl_merges + start_ttl_merges + stop_moves + start_moves + wait_loading_parts + stop_fetches + start_fetches
               + stop_replicated_sends + start_replicated_sends + stop_replication_queues + start_replication_queues
               + stop_pulling_replication_log + start_pulling_replication_log + sync_replica + sync_replicated_database + restart_replica
               + restore_replica + restart_replicas + sync_file_cache + drop_filesystem_cache + load_pks + load_pk + unload_pks + 1))
    {
        sc->set_unload_pks(true);
    }
    else if (
        unload_pk
        && nopt
            < (reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + reload_function
               + reload_asynchronous_metrics + drop_dns_cache + drop_mark_cache + drop_uncompressed_cache + drop_compiled_expression_cache
               + drop_query_cache + drop_format_schema_cache + flush_logs + reload_config + reload_users + stop_merges + start_merges
               + stop_ttl_merges + start_ttl_merges + stop_moves + start_moves + wait_loading_parts + stop_fetches + start_fetches
               + stop_replicated_sends + start_replicated_sends + stop_replication_queues + start_replication_queues
               + stop_pulling_replication_log + start_pulling_replication_log + sync_replica + sync_replicated_database + restart_replica
               + restore_replica + restart_replicas + sync_file_cache + drop_filesystem_cache + load_pks + load_pk + unload_pks + unload_pk
               + 1))
    {
        setTableSystemStatement<SQLTable>(rg, has_merge_tree_func, sc->mutable_unload_pk());
    }
    else if (
        refresh_views
        && nopt
            < (reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + reload_function
               + reload_asynchronous_metrics + drop_dns_cache + drop_mark_cache + drop_uncompressed_cache + drop_compiled_expression_cache
               + drop_query_cache + drop_format_schema_cache + flush_logs + reload_config + reload_users + stop_merges + start_merges
               + stop_ttl_merges + start_ttl_merges + stop_moves + start_moves + wait_loading_parts + stop_fetches + start_fetches
               + stop_replicated_sends + start_replicated_sends + stop_replication_queues + start_replication_queues
               + stop_pulling_replication_log + start_pulling_replication_log + sync_replica + sync_replicated_database + restart_replica
               + restore_replica + restart_replicas + sync_file_cache + drop_filesystem_cache + load_pks + load_pk + unload_pks + unload_pk
               + refresh_views + 1))
    {
        sc->set_refresh_views(true);
    }
    else if (
        refresh_view
        && nopt
            < (reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + reload_function
               + reload_asynchronous_metrics + drop_dns_cache + drop_mark_cache + drop_uncompressed_cache + drop_compiled_expression_cache
               + drop_query_cache + drop_format_schema_cache + flush_logs + reload_config + reload_users + stop_merges + start_merges
               + stop_ttl_merges + start_ttl_merges + stop_moves + start_moves + wait_loading_parts + stop_fetches + start_fetches
               + stop_replicated_sends + start_replicated_sends + stop_replication_queues + start_replication_queues
               + stop_pulling_replication_log + start_pulling_replication_log + sync_replica + sync_replicated_database + restart_replica
               + restore_replica + restart_replicas + sync_file_cache + drop_filesystem_cache + load_pks + load_pk + unload_pks + unload_pk
               + refresh_views + refresh_view + 1))
    {
        setTableSystemStatement<SQLView>(rg, has_refreshable_view_func, sc->mutable_refresh_view());
    }
    else if (
        stop_views
        && nopt
            < (reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + reload_function
               + reload_asynchronous_metrics + drop_dns_cache + drop_mark_cache + drop_uncompressed_cache + drop_compiled_expression_cache
               + drop_query_cache + drop_format_schema_cache + flush_logs + reload_config + reload_users + stop_merges + start_merges
               + stop_ttl_merges + start_ttl_merges + stop_moves + start_moves + wait_loading_parts + stop_fetches + start_fetches
               + stop_replicated_sends + start_replicated_sends + stop_replication_queues + start_replication_queues
               + stop_pulling_replication_log + start_pulling_replication_log + sync_replica + sync_replicated_database + restart_replica
               + restore_replica + restart_replicas + sync_file_cache + drop_filesystem_cache + load_pks + load_pk + unload_pks + unload_pk
               + refresh_views + refresh_view + stop_views + 1))
    {
        sc->set_stop_views(true);
    }
    else if (
        stop_view
        && nopt
            < (reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + reload_function
               + reload_asynchronous_metrics + drop_dns_cache + drop_mark_cache + drop_uncompressed_cache + drop_compiled_expression_cache
               + drop_query_cache + drop_format_schema_cache + flush_logs + reload_config + reload_users + stop_merges + start_merges
               + stop_ttl_merges + start_ttl_merges + stop_moves + start_moves + wait_loading_parts + stop_fetches + start_fetches
               + stop_replicated_sends + start_replicated_sends + stop_replication_queues + start_replication_queues
               + stop_pulling_replication_log + start_pulling_replication_log + sync_replica + sync_replicated_database + restart_replica
               + restore_replica + restart_replicas + sync_file_cache + drop_filesystem_cache + load_pks + load_pk + unload_pks + unload_pk
               + refresh_views + refresh_view + stop_views + stop_view + 1))
    {
        setTableSystemStatement<SQLView>(rg, has_refreshable_view_func, sc->mutable_stop_view());
    }
    else if (
        start_views
        && nopt
            < (reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + reload_function
               + reload_asynchronous_metrics + drop_dns_cache + drop_mark_cache + drop_uncompressed_cache + drop_compiled_expression_cache
               + drop_query_cache + drop_format_schema_cache + flush_logs + reload_config + reload_users + stop_merges + start_merges
               + stop_ttl_merges + start_ttl_merges + stop_moves + start_moves + wait_loading_parts + stop_fetches + start_fetches
               + stop_replicated_sends + start_replicated_sends + stop_replication_queues + start_replication_queues
               + stop_pulling_replication_log + start_pulling_replication_log + sync_replica + sync_replicated_database + restart_replica
               + restore_replica + restart_replicas + sync_file_cache + drop_filesystem_cache + load_pks + load_pk + unload_pks + unload_pk
               + refresh_views + refresh_view + stop_views + stop_view + start_views + 1))
    {
        sc->set_start_views(true);
    }
    else if (
        start_view
        && nopt
            < (reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + reload_function
               + reload_asynchronous_metrics + drop_dns_cache + drop_mark_cache + drop_uncompressed_cache + drop_compiled_expression_cache
               + drop_query_cache + drop_format_schema_cache + flush_logs + reload_config + reload_users + stop_merges + start_merges
               + stop_ttl_merges + start_ttl_merges + stop_moves + start_moves + wait_loading_parts + stop_fetches + start_fetches
               + stop_replicated_sends + start_replicated_sends + stop_replication_queues + start_replication_queues
               + stop_pulling_replication_log + start_pulling_replication_log + sync_replica + sync_replicated_database + restart_replica
               + restore_replica + restart_replicas + sync_file_cache + drop_filesystem_cache + load_pks + load_pk + unload_pks + unload_pk
               + refresh_views + refresh_view + stop_views + stop_view + start_views + start_view + 1))
    {
        setTableSystemStatement<SQLView>(rg, has_refreshable_view_func, sc->mutable_start_view());
    }
    else if (
        cancel_view
        && nopt
            < (reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + reload_function
               + reload_asynchronous_metrics + drop_dns_cache + drop_mark_cache + drop_uncompressed_cache + drop_compiled_expression_cache
               + drop_query_cache + drop_format_schema_cache + flush_logs + reload_config + reload_users + stop_merges + start_merges
               + stop_ttl_merges + start_ttl_merges + stop_moves + start_moves + wait_loading_parts + stop_fetches + start_fetches
               + stop_replicated_sends + start_replicated_sends + stop_replication_queues + start_replication_queues
               + stop_pulling_replication_log + start_pulling_replication_log + sync_replica + sync_replicated_database + restart_replica
               + restore_replica + restart_replicas + sync_file_cache + drop_filesystem_cache + load_pks + load_pk + unload_pks + unload_pk
               + refresh_views + refresh_view + stop_views + stop_view + start_views + start_view + cancel_view + 1))
    {
        setTableSystemStatement<SQLView>(rg, has_refreshable_view_func, sc->mutable_cancel_view());
    }
    else if (
        wait_view
        && nopt
            < (reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + reload_function
               + reload_asynchronous_metrics + drop_dns_cache + drop_mark_cache + drop_uncompressed_cache + drop_compiled_expression_cache
               + drop_query_cache + drop_format_schema_cache + flush_logs + reload_config + reload_users + stop_merges + start_merges
               + stop_ttl_merges + start_ttl_merges + stop_moves + start_moves + wait_loading_parts + stop_fetches + start_fetches
               + stop_replicated_sends + start_replicated_sends + stop_replication_queues + start_replication_queues
               + stop_pulling_replication_log + start_pulling_replication_log + sync_replica + sync_replicated_database + restart_replica
               + restore_replica + restart_replicas + sync_file_cache + drop_filesystem_cache + load_pks + load_pk + unload_pks + unload_pk
               + refresh_views + refresh_view + stop_views + stop_view + start_views + start_view + cancel_view + wait_view + 1))
    {
        setTableSystemStatement<SQLView>(rg, has_refreshable_view_func, sc->mutable_wait_view());
    }
    else if (
        prewarm_cache
        && nopt
            < (reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + reload_function
               + reload_asynchronous_metrics + drop_dns_cache + drop_mark_cache + drop_uncompressed_cache + drop_compiled_expression_cache
               + drop_query_cache + drop_format_schema_cache + flush_logs + reload_config + reload_users + stop_merges + start_merges
               + stop_ttl_merges + start_ttl_merges + stop_moves + start_moves + wait_loading_parts + stop_fetches + start_fetches
               + stop_replicated_sends + start_replicated_sends + stop_replication_queues + start_replication_queues
               + stop_pulling_replication_log + start_pulling_replication_log + sync_replica + sync_replicated_database + restart_replica
               + restore_replica + restart_replicas + sync_file_cache + drop_filesystem_cache + load_pks + load_pk + unload_pks + unload_pk
               + refresh_views + refresh_view + stop_views + stop_view + start_views + start_view + cancel_view + wait_view + prewarm_cache
               + 1))
    {
        setTableSystemStatement<SQLTable>(rg, has_merge_tree_func, sc->mutable_prewarm_cache());
    }
    else if (
        prewarm_primary_index_cache
        && nopt
            < (reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + reload_function
               + reload_asynchronous_metrics + drop_dns_cache + drop_mark_cache + drop_uncompressed_cache + drop_compiled_expression_cache
               + drop_query_cache + drop_format_schema_cache + flush_logs + reload_config + reload_users + stop_merges + start_merges
               + stop_ttl_merges + start_ttl_merges + stop_moves + start_moves + wait_loading_parts + stop_fetches + start_fetches
               + stop_replicated_sends + start_replicated_sends + stop_replication_queues + start_replication_queues
               + stop_pulling_replication_log + start_pulling_replication_log + sync_replica + sync_replicated_database + restart_replica
               + restore_replica + restart_replicas + sync_file_cache + drop_filesystem_cache + load_pks + load_pk + unload_pks + unload_pk
               + refresh_views + refresh_view + stop_views + stop_view + start_views + start_view + cancel_view + wait_view + prewarm_cache
               + prewarm_primary_index_cache + 1))
    {
        setTableSystemStatement<SQLTable>(rg, has_merge_tree_func, sc->mutable_prewarm_primary_index_cache());
    }
    else if (
        drop_connections_cache
        && nopt
            < (reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + reload_function
               + reload_asynchronous_metrics + drop_dns_cache + drop_mark_cache + drop_uncompressed_cache + drop_compiled_expression_cache
               + drop_query_cache + drop_format_schema_cache + flush_logs + reload_config + reload_users + stop_merges + start_merges
               + stop_ttl_merges + start_ttl_merges + stop_moves + start_moves + wait_loading_parts + stop_fetches + start_fetches
               + stop_replicated_sends + start_replicated_sends + stop_replication_queues + start_replication_queues
               + stop_pulling_replication_log + start_pulling_replication_log + sync_replica + sync_replicated_database + restart_replica
               + restore_replica + restart_replicas + sync_file_cache + drop_filesystem_cache + load_pks + load_pk + unload_pks + unload_pk
               + refresh_views + refresh_view + stop_views + stop_view + start_views + start_view + cancel_view + wait_view + prewarm_cache
               + prewarm_primary_index_cache + drop_connections_cache + 1))
    {
        sc->set_drop_connections_cache(true);
    }
    else if (
        drop_primary_index_cache
        && nopt
            < (reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + reload_function
               + reload_asynchronous_metrics + drop_dns_cache + drop_mark_cache + drop_uncompressed_cache + drop_compiled_expression_cache
               + drop_query_cache + drop_format_schema_cache + flush_logs + reload_config + reload_users + stop_merges + start_merges
               + stop_ttl_merges + start_ttl_merges + stop_moves + start_moves + wait_loading_parts + stop_fetches + start_fetches
               + stop_replicated_sends + start_replicated_sends + stop_replication_queues + start_replication_queues
               + stop_pulling_replication_log + start_pulling_replication_log + sync_replica + sync_replicated_database + restart_replica
               + restore_replica + restart_replicas + sync_file_cache + drop_filesystem_cache + load_pks + load_pk + unload_pks + unload_pk
               + refresh_views + refresh_view + stop_views + stop_view + start_views + start_view + cancel_view + wait_view + prewarm_cache
               + prewarm_primary_index_cache + drop_connections_cache + drop_primary_index_cache + 1))
    {
        sc->set_drop_primary_index_cache(true);
    }
    else if (
        drop_index_mark_cache
        && nopt
            < (reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + reload_function
               + reload_asynchronous_metrics + drop_dns_cache + drop_mark_cache + drop_uncompressed_cache + drop_compiled_expression_cache
               + drop_query_cache + drop_format_schema_cache + flush_logs + reload_config + reload_users + stop_merges + start_merges
               + stop_ttl_merges + start_ttl_merges + stop_moves + start_moves + wait_loading_parts + stop_fetches + start_fetches
               + stop_replicated_sends + start_replicated_sends + stop_replication_queues + start_replication_queues
               + stop_pulling_replication_log + start_pulling_replication_log + sync_replica + sync_replicated_database + restart_replica
               + restore_replica + restart_replicas + sync_file_cache + drop_filesystem_cache + load_pks + load_pk + unload_pks + unload_pk
               + refresh_views + refresh_view + stop_views + stop_view + start_views + start_view + cancel_view + wait_view + prewarm_cache
               + prewarm_primary_index_cache + drop_connections_cache + drop_primary_index_cache + drop_index_mark_cache + 1))
    {
        sc->set_drop_index_mark_cache(true);
    }
    else if (
        drop_index_uncompressed_cache
        && nopt
            < (reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + reload_function
               + reload_asynchronous_metrics + drop_dns_cache + drop_mark_cache + drop_uncompressed_cache + drop_compiled_expression_cache
               + drop_query_cache + drop_format_schema_cache + flush_logs + reload_config + reload_users + stop_merges + start_merges
               + stop_ttl_merges + start_ttl_merges + stop_moves + start_moves + wait_loading_parts + stop_fetches + start_fetches
               + stop_replicated_sends + start_replicated_sends + stop_replication_queues + start_replication_queues
               + stop_pulling_replication_log + start_pulling_replication_log + sync_replica + sync_replicated_database + restart_replica
               + restore_replica + restart_replicas + sync_file_cache + drop_filesystem_cache + load_pks + load_pk + unload_pks + unload_pk
               + refresh_views + refresh_view + stop_views + stop_view + start_views + start_view + cancel_view + wait_view + prewarm_cache
               + prewarm_primary_index_cache + drop_connections_cache + drop_primary_index_cache + drop_index_mark_cache
               + drop_index_uncompressed_cache + 1))
    {
        sc->set_drop_index_uncompressed_cache(true);
    }
    else if (
        drop_mmap_cache
        && nopt
            < (reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + reload_function
               + reload_asynchronous_metrics + drop_dns_cache + drop_mark_cache + drop_uncompressed_cache + drop_compiled_expression_cache
               + drop_query_cache + drop_format_schema_cache + flush_logs + reload_config + reload_users + stop_merges + start_merges
               + stop_ttl_merges + start_ttl_merges + stop_moves + start_moves + wait_loading_parts + stop_fetches + start_fetches
               + stop_replicated_sends + start_replicated_sends + stop_replication_queues + start_replication_queues
               + stop_pulling_replication_log + start_pulling_replication_log + sync_replica + sync_replicated_database + restart_replica
               + restore_replica + restart_replicas + sync_file_cache + drop_filesystem_cache + load_pks + load_pk + unload_pks + unload_pk
               + refresh_views + refresh_view + stop_views + stop_view + start_views + start_view + cancel_view + wait_view + prewarm_cache
               + prewarm_primary_index_cache + drop_connections_cache + drop_primary_index_cache + drop_index_mark_cache
               + drop_index_uncompressed_cache + drop_mmap_cache + 1))
    {
        sc->set_drop_mmap_cache(true);
    }
    else if (
        drop_page_cache
        && nopt
            < (reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + reload_function
               + reload_asynchronous_metrics + drop_dns_cache + drop_mark_cache + drop_uncompressed_cache + drop_compiled_expression_cache
               + drop_query_cache + drop_format_schema_cache + flush_logs + reload_config + reload_users + stop_merges + start_merges
               + stop_ttl_merges + start_ttl_merges + stop_moves + start_moves + wait_loading_parts + stop_fetches + start_fetches
               + stop_replicated_sends + start_replicated_sends + stop_replication_queues + start_replication_queues
               + stop_pulling_replication_log + start_pulling_replication_log + sync_replica + sync_replicated_database + restart_replica
               + restore_replica + restart_replicas + sync_file_cache + drop_filesystem_cache + load_pks + load_pk + unload_pks + unload_pk
               + refresh_views + refresh_view + stop_views + stop_view + start_views + start_view + cancel_view + wait_view + prewarm_cache
               + prewarm_primary_index_cache + drop_connections_cache + drop_primary_index_cache + drop_index_mark_cache
               + drop_index_uncompressed_cache + drop_mmap_cache + drop_page_cache + 1))
    {
        sc->set_drop_page_cache(true);
    }
    else if (
        drop_schema_cache
        && nopt
            < (reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + reload_function
               + reload_asynchronous_metrics + drop_dns_cache + drop_mark_cache + drop_uncompressed_cache + drop_compiled_expression_cache
               + drop_query_cache + drop_format_schema_cache + flush_logs + reload_config + reload_users + stop_merges + start_merges
               + stop_ttl_merges + start_ttl_merges + stop_moves + start_moves + wait_loading_parts + stop_fetches + start_fetches
               + stop_replicated_sends + start_replicated_sends + stop_replication_queues + start_replication_queues
               + stop_pulling_replication_log + start_pulling_replication_log + sync_replica + sync_replicated_database + restart_replica
               + restore_replica + restart_replicas + sync_file_cache + drop_filesystem_cache + load_pks + load_pk + unload_pks + unload_pk
               + refresh_views + refresh_view + stop_views + stop_view + start_views + start_view + cancel_view + wait_view + prewarm_cache
               + prewarm_primary_index_cache + drop_connections_cache + drop_primary_index_cache + drop_index_mark_cache
               + drop_index_uncompressed_cache + drop_mmap_cache + drop_page_cache + drop_schema_cache + 1))
    {
        sc->set_drop_schema_cache(true);
    }
    else if (
        drop_s3_client_cache
        && nopt
            < (reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + reload_function
               + reload_asynchronous_metrics + drop_dns_cache + drop_mark_cache + drop_uncompressed_cache + drop_compiled_expression_cache
               + drop_query_cache + drop_format_schema_cache + flush_logs + reload_config + reload_users + stop_merges + start_merges
               + stop_ttl_merges + start_ttl_merges + stop_moves + start_moves + wait_loading_parts + stop_fetches + start_fetches
               + stop_replicated_sends + start_replicated_sends + stop_replication_queues + start_replication_queues
               + stop_pulling_replication_log + start_pulling_replication_log + sync_replica + sync_replicated_database + restart_replica
               + restore_replica + restart_replicas + sync_file_cache + drop_filesystem_cache + load_pks + load_pk + unload_pks + unload_pk
               + refresh_views + refresh_view + stop_views + stop_view + start_views + start_view + cancel_view + wait_view + prewarm_cache
               + prewarm_primary_index_cache + drop_connections_cache + drop_primary_index_cache + drop_index_mark_cache
               + drop_index_uncompressed_cache + drop_mmap_cache + drop_page_cache + drop_schema_cache + drop_s3_client_cache + 1))
    {
        sc->set_drop_s3_client_cache(true);
    }
    else if (
        flush_async_insert_queue
        && nopt
            < (reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + reload_function
               + reload_asynchronous_metrics + drop_dns_cache + drop_mark_cache + drop_uncompressed_cache + drop_compiled_expression_cache
               + drop_query_cache + drop_format_schema_cache + flush_logs + reload_config + reload_users + stop_merges + start_merges
               + stop_ttl_merges + start_ttl_merges + stop_moves + start_moves + wait_loading_parts + stop_fetches + start_fetches
               + stop_replicated_sends + start_replicated_sends + stop_replication_queues + start_replication_queues
               + stop_pulling_replication_log + start_pulling_replication_log + sync_replica + sync_replicated_database + restart_replica
               + restore_replica + restart_replicas + sync_file_cache + drop_filesystem_cache + load_pks + load_pk + unload_pks + unload_pk
               + refresh_views + refresh_view + stop_views + stop_view + start_views + start_view + cancel_view + wait_view + prewarm_cache
               + prewarm_primary_index_cache + drop_connections_cache + drop_primary_index_cache + drop_index_mark_cache
               + drop_index_uncompressed_cache + drop_mmap_cache + drop_page_cache + drop_schema_cache + drop_s3_client_cache
               + flush_async_insert_queue + 1))
    {
        sc->set_flush_async_insert_queue(true);
    }
    else if (
        sync_filesystem_cache
        && nopt
            < (reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + reload_function
               + reload_asynchronous_metrics + drop_dns_cache + drop_mark_cache + drop_uncompressed_cache + drop_compiled_expression_cache
               + drop_query_cache + drop_format_schema_cache + flush_logs + reload_config + reload_users + stop_merges + start_merges
               + stop_ttl_merges + start_ttl_merges + stop_moves + start_moves + wait_loading_parts + stop_fetches + start_fetches
               + stop_replicated_sends + start_replicated_sends + stop_replication_queues + start_replication_queues
               + stop_pulling_replication_log + start_pulling_replication_log + sync_replica + sync_replicated_database + restart_replica
               + restore_replica + restart_replicas + sync_file_cache + drop_filesystem_cache + load_pks + load_pk + unload_pks + unload_pk
               + refresh_views + refresh_view + stop_views + stop_view + start_views + start_view + cancel_view + wait_view + prewarm_cache
               + prewarm_primary_index_cache + drop_connections_cache + drop_primary_index_cache + drop_index_mark_cache
               + drop_index_uncompressed_cache + drop_mmap_cache + drop_page_cache + drop_schema_cache + drop_s3_client_cache
               + flush_async_insert_queue + sync_filesystem_cache + 1))
    {
        sc->set_sync_filesystem_cache(true);
    }
    else if (
        drop_cache
        && nopt
            < (reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + reload_function
               + reload_asynchronous_metrics + drop_dns_cache + drop_mark_cache + drop_uncompressed_cache + drop_compiled_expression_cache
               + drop_query_cache + drop_format_schema_cache + flush_logs + reload_config + reload_users + stop_merges + start_merges
               + stop_ttl_merges + start_ttl_merges + stop_moves + start_moves + wait_loading_parts + stop_fetches + start_fetches
               + stop_replicated_sends + start_replicated_sends + stop_replication_queues + start_replication_queues
               + stop_pulling_replication_log + start_pulling_replication_log + sync_replica + sync_replicated_database + restart_replica
               + restore_replica + restart_replicas + sync_file_cache + drop_filesystem_cache + load_pks + load_pk + unload_pks + unload_pk
               + refresh_views + refresh_view + stop_views + stop_view + start_views + start_view + cancel_view + wait_view + prewarm_cache
               + prewarm_primary_index_cache + drop_connections_cache + drop_primary_index_cache + drop_index_mark_cache
               + drop_index_uncompressed_cache + drop_mmap_cache + drop_page_cache + drop_schema_cache + drop_s3_client_cache
               + flush_async_insert_queue + sync_filesystem_cache + drop_cache + 1))
    {
        sc->set_drop_cache(true);
    }
    else if (
        drop_skip_index_cache
        && nopt
            < (reload_embedded_dictionaries + reload_dictionaries + reload_models + reload_functions + reload_function
               + reload_asynchronous_metrics + drop_dns_cache + drop_mark_cache + drop_uncompressed_cache + drop_compiled_expression_cache
               + drop_query_cache + drop_format_schema_cache + flush_logs + reload_config + reload_users + stop_merges + start_merges
               + stop_ttl_merges + start_ttl_merges + stop_moves + start_moves + wait_loading_parts + stop_fetches + start_fetches
               + stop_replicated_sends + start_replicated_sends + stop_replication_queues + start_replication_queues
               + stop_pulling_replication_log + start_pulling_replication_log + sync_replica + sync_replicated_database + restart_replica
               + restore_replica + restart_replicas + sync_file_cache + drop_filesystem_cache + load_pks + load_pk + unload_pks + unload_pk
               + refresh_views + refresh_view + stop_views + stop_view + start_views + start_view + cancel_view + wait_view + prewarm_cache
               + prewarm_primary_index_cache + drop_connections_cache + drop_primary_index_cache + drop_index_mark_cache
               + drop_index_uncompressed_cache + drop_mmap_cache + drop_page_cache + drop_schema_cache + drop_s3_client_cache
               + flush_async_insert_queue + sync_filesystem_cache + drop_cache + drop_skip_index_cache + 1))
    {
        sc->set_drop_skip_index_cache(true);
    }
    else
    {
        chassert(0);
    }
}

static std::optional<String> backupOrRestoreObject(BackupRestoreObject * bro, const SQLObject obj, const SQLBase & b)
{
    bro->set_is_temp(b.is_temp);
    bro->set_sobject(obj);
    return b.getCluster();
}

static void backupOrRestoreSystemTable(BackupRestoreObject * bro, const String & name)
{
    ExprSchemaTable * est = bro->mutable_object()->mutable_est();

    bro->set_sobject(SQLObject::TABLE);
    est->mutable_database()->set_database("system");
    est->mutable_table()->set_table(name);
}

static std::optional<String> backupOrRestoreDatabase(BackupRestoreObject * bro, const std::shared_ptr<SQLDatabase> & d)
{
    bro->set_sobject(SQLObject::DATABASE);
    d->setName(bro->mutable_object()->mutable_database());
    return d->getCluster();
}

void StatementGenerator::generateNextBackup(RandomGenerator & rg, BackupRestore * br)
{
    const uint32_t backup_table = 10 * static_cast<uint32_t>(collectionHas<SQLTable>(attached_tables));
    const uint32_t backup_system_table = 3 * static_cast<uint32_t>(!systemTables.empty());
    const uint32_t backup_view = 10 * static_cast<uint32_t>(collectionHas<SQLView>(attached_views));
    const uint32_t backup_dictionary = 10 * static_cast<uint32_t>(collectionHas<SQLDictionary>(attached_dictionaries));
    const uint32_t backup_database = 10 * static_cast<uint32_t>(collectionHas<std::shared_ptr<SQLDatabase>>(attached_databases));
    const uint32_t all_temporary = 3;
    const uint32_t everything = 3;
    const uint32_t prob_space = backup_table + backup_system_table + backup_view + backup_dictionary + backup_database;
    std::uniform_int_distribution<uint32_t> next_dist(1, prob_space);
    const uint32_t nopt = next_dist(rg.generator);
    BackupRestoreElement * bre = br->mutable_backup_element();
    std::optional<String> cluster;

    br->set_command(BackupRestore_BackupCommand_BACKUP);
    if (backup_table && nopt < (backup_table + 1))
    {
        BackupRestoreObject * bro = bre->mutable_bobject();
        const SQLTable & t = rg.pickRandomly(filterCollection<SQLTable>(attached_tables));
        const String dname = t.db ? ("d" + std::to_string(t.db->dname)) : "";
        const String tname = "t" + std::to_string(t.tname);
        const bool table_has_partitions = t.isMergeTreeFamily() && fc.tableHasPartitions(false, dname, tname);

        t.setName(bro->mutable_object()->mutable_est(), false);
        cluster = backupOrRestoreObject(bro, SQLObject::TABLE, t);
        if (table_has_partitions && rg.nextSmallNumber() < 4)
        {
            bro->add_partitions()->set_partition_id(fc.tableGetRandomPartitionOrPart(false, true, dname, tname));
        }
    }
    else if (backup_system_table && nopt < (backup_table + backup_system_table + 1))
    {
        backupOrRestoreSystemTable(bre->mutable_bobject(), rg.pickRandomly(systemTables));
    }
    else if (backup_view && nopt < (backup_table + backup_system_table + backup_view + 1))
    {
        BackupRestoreObject * bro = bre->mutable_bobject();
        const SQLView & v = rg.pickRandomly(filterCollection<SQLView>(attached_views));

        v.setName(bro->mutable_object()->mutable_est(), false);
        cluster = backupOrRestoreObject(bro, SQLObject::VIEW, v);
    }
    else if (backup_dictionary && nopt < (backup_table + backup_system_table + backup_view + backup_dictionary + 1))
    {
        BackupRestoreObject * bro = bre->mutable_bobject();
        const SQLDictionary & d = rg.pickRandomly(filterCollection<SQLDictionary>(attached_dictionaries));

        d.setName(bro->mutable_object()->mutable_est(), false);
        cluster = backupOrRestoreObject(bro, SQLObject::DICTIONARY, d);
    }
    else if (backup_database && nopt < (backup_table + backup_system_table + backup_view + backup_dictionary + backup_database + 1))
    {
        cluster = backupOrRestoreDatabase(
            bre->mutable_bobject(), rg.pickRandomly(filterCollection<std::shared_ptr<SQLDatabase>>(attached_databases)));
    }
    else if (
        all_temporary
        && nopt < (backup_table + backup_system_table + backup_view + backup_dictionary + backup_database + all_temporary + 1))
    {
        bre->set_all_temporary(true);
    }
    else if (
        everything
        && nopt < (backup_table + backup_system_table + backup_view + backup_dictionary + backup_database + all_temporary + everything + 1))
    {
        bre->set_all(true);
    }
    else
    {
        chassert(0);
    }
    if (cluster.has_value())
    {
        br->mutable_cluster()->set_cluster(cluster.value());
    }

    const uint32_t out_to_disk = 10 * static_cast<uint32_t>(!fc.disks.empty());
    const uint32_t out_to_file = 10;
    const uint32_t out_to_s3 = 10 * static_cast<uint32_t>(connections.hasMinIOConnection());
    const uint32_t out_to_memory = 5;
    const uint32_t out_to_null = 3;
    const uint32_t prob_space2 = out_to_disk + out_to_file + out_to_s3 + out_to_memory + out_to_null;
    std::uniform_int_distribution<uint32_t> next_dist2(1, prob_space2);
    const uint32_t nopt2 = next_dist2(rg.generator);
    String backup_file = "backup";
    BackupRestore_BackupOutput outf = BackupRestore_BackupOutput_Null;

    br->set_backup_number(backup_counter++);
    /// Set backup file
    if (nopt2 < (out_to_disk + out_to_file + out_to_s3 + out_to_memory + 1))
    {
        backup_file += std::to_string(br->backup_number());
    }
    if (nopt2 < (out_to_disk + out_to_file + out_to_s3 + 1) && rg.nextBool())
    {
        static const DB::Strings & backupFormats = {"tar", "zip", "tzst", "tgz"};
        const String & nsuffix = rg.pickRandomly(backupFormats);

        backup_file += ".";
        backup_file += nsuffix;
        if (nsuffix == "tar" && rg.nextBool())
        {
            static const DB::Strings & tarSuffixes = {"gz", "bz2", "lzma", "zst", "xz"};

            backup_file += ".";
            backup_file += rg.pickRandomly(tarSuffixes);
        }
    }
    if (out_to_disk && (nopt2 < out_to_disk + 1))
    {
        outf = BackupRestore_BackupOutput_Disk;
        br->add_out_params(rg.pickRandomly(fc.disks));
        br->add_out_params(std::move(backup_file));
    }
    else if (out_to_file && (nopt2 < out_to_disk + out_to_file + 1))
    {
        outf = BackupRestore_BackupOutput_File;
        br->add_out_params((fc.db_file_path / std::move(backup_file)).generic_string());
    }
    else if (out_to_s3 && (nopt2 < out_to_disk + out_to_file + out_to_s3 + 1))
    {
        outf = BackupRestore_BackupOutput_S3;
        connections.setBackupDetails((fc.db_file_path / std::move(backup_file)).generic_string(), br);
    }
    else if (out_to_memory && nopt2 < (out_to_disk + out_to_file + out_to_s3 + out_to_memory + 1))
    {
        outf = BackupRestore_BackupOutput_Memory;
        br->add_out_params(std::move(backup_file));
    }
    br->set_out(outf);
    if (rg.nextSmallNumber() < 4)
    {
        br->set_format(static_cast<OutFormat>((rg.nextRandomUInt32() % static_cast<uint32_t>(OutFormat_MAX)) + 1));
    }
}

void StatementGenerator::generateNextRestore(RandomGenerator & rg, BackupRestore * br)
{
    const CatalogBackup & backup = rg.pickValueRandomlyFromMap(backups);
    BackupRestoreElement * bre = br->mutable_backup_element();
    std::optional<String> cluster;

    br->set_command(BackupRestore_BackupCommand_RESTORE);
    if (backup.all_temporary)
    {
        bre->set_all_temporary(true);
    }
    else if (backup.everything)
    {
        bre->set_all(true);
    }
    else
    {
        const uint32_t restore_table = 10 * static_cast<uint32_t>(!backup.tables.empty());
        const uint32_t restore_system_table = 3 * static_cast<uint32_t>(backup.system_table.has_value());
        const uint32_t restore_view = 10 * static_cast<uint32_t>(!backup.views.empty());
        const uint32_t restore_dictionary = 10 * static_cast<uint32_t>(!backup.dictionaries.empty());
        const uint32_t restore_database = 10 * static_cast<uint32_t>(!backup.databases.empty());
        const uint32_t prob_space = restore_table + restore_system_table + restore_view + restore_dictionary + restore_database;
        std::uniform_int_distribution<uint32_t> next_dist(1, prob_space);
        const uint32_t nopt = next_dist(rg.generator);

        if (restore_table && (nopt < restore_table + 1))
        {
            BackupRestoreObject * bro = bre->mutable_bobject();
            const SQLTable & t = rg.pickValueRandomlyFromMap(backup.tables);

            t.setName(bro->mutable_object()->mutable_est(), false);
            cluster = backupOrRestoreObject(bro, SQLObject::TABLE, t);
            if (backup.partition_id.has_value() && rg.nextSmallNumber() < 4)
            {
                bro->add_partitions()->set_partition_id(backup.partition_id.value());
            }
        }
        else if (restore_system_table && (nopt < restore_table + restore_system_table + 1))
        {
            backupOrRestoreSystemTable(bre->mutable_bobject(), backup.system_table.value());
        }
        else if (restore_view && nopt < (restore_table + restore_system_table + restore_view + 1))
        {
            BackupRestoreObject * bro = bre->mutable_bobject();
            const SQLView & v = rg.pickValueRandomlyFromMap(backup.views);

            v.setName(bro->mutable_object()->mutable_est(), false);
            cluster = backupOrRestoreObject(bro, SQLObject::VIEW, v);
        }
        else if (restore_dictionary && nopt < (restore_table + restore_system_table + restore_view + restore_dictionary + 1))
        {
            BackupRestoreObject * bro = bre->mutable_bobject();
            const SQLDictionary & d = rg.pickValueRandomlyFromMap(backup.dictionaries);

            d.setName(bro->mutable_object()->mutable_est(), false);
            cluster = backupOrRestoreObject(bro, SQLObject::DICTIONARY, d);
        }
        else if (
            restore_database && nopt < (restore_table + restore_system_table + restore_view + restore_dictionary + restore_database + 1))
        {
            cluster = backupOrRestoreDatabase(bre->mutable_bobject(), rg.pickValueRandomlyFromMap(backup.databases));
        }
        else
        {
            chassert(0);
        }
    }

    if (cluster.has_value())
    {
        br->mutable_cluster()->set_cluster(cluster.value());
    }
    br->set_out(backup.outf);
    for (const auto & entry : backup.out_params)
    {
        br->add_out_params(entry);
    }
    if (backup.out_format.has_value())
    {
        br->set_format(backup.out_format.value());
    }
    br->set_backup_number(backup.backup_num);
}

void StatementGenerator::generateNextBackupOrRestore(RandomGenerator & rg, BackupRestore * br)
{
    SettingValues * vals = nullptr;
    const bool isBackup = backups.empty() || rg.nextBool();

    if (isBackup)
    {
        generateNextBackup(rg, br);
    }
    else
    {
        generateNextRestore(rg, br);
    }
    if (rg.nextSmallNumber() < 4)
    {
        vals = vals ? vals : br->mutable_setting_values();
        generateSettingValues(rg, isBackup ? backupSettings : restoreSettings, vals);
    }
    if (isBackup && !backups.empty() && rg.nextBool())
    {
        /// Do an incremental backup
        String info;
        vals = vals ? vals : br->mutable_setting_values();
        SetValue * sv = vals->has_set_value() ? vals->add_other_values() : vals->mutable_set_value();
        const CatalogBackup & backup = rg.pickValueRandomlyFromMap(backups);

        sv->set_property("base_backup");
        info += BackupRestore_BackupOutput_Name(backup.outf);
        info += "(";
        for (size_t i = 0; i < backup.out_params.size(); i++)
        {
            if (i != 0)
            {
                info += ", ";
            }
            info += "'";
            info += backup.out_params[i];
            info += "'";
        }
        info += ")";
        sv->set_value(std::move(info));
    }
    if (rg.nextSmallNumber() < 4)
    {
        vals = vals ? vals : br->mutable_setting_values();
        generateSettingValues(rg, serverSettings, vals);
    }
    br->set_async(rg.nextSmallNumber() < 4);
}

void StatementGenerator::generateNextQuery(RandomGenerator & rg, SQLQueryInner * sq)
{
    const bool has_databases = collectionHas<std::shared_ptr<SQLDatabase>>(attached_databases);
    const bool has_tables = collectionHas<SQLTable>(attached_tables);

    const uint32_t create_table = 6 * static_cast<uint32_t>(static_cast<uint32_t>(tables.size()) < this->fc.max_tables);
    const uint32_t create_view = 10 * static_cast<uint32_t>(static_cast<uint32_t>(views.size()) < this->fc.max_views);
    const uint32_t drop = 2
        * static_cast<uint32_t>(
                              collectionCount<SQLTable>(attached_tables) > 3 || collectionCount<SQLView>(attached_views) > 3
                              || collectionCount<SQLDictionary>(attached_dictionaries) > 3
                              || collectionCount<std::shared_ptr<SQLDatabase>>(attached_databases) > 3 || functions.size() > 3);
    const uint32_t insert = 180 * static_cast<uint32_t>(has_tables);
    const uint32_t light_delete = 6 * static_cast<uint32_t>(has_tables);
    const uint32_t truncate = 2 * static_cast<uint32_t>(has_databases || has_tables);
    const uint32_t optimize_table = 2 * static_cast<uint32_t>(collectionHas<SQLTable>(optimize_table_lambda));
    const uint32_t check_table = 2 * static_cast<uint32_t>(has_tables);
    const uint32_t desc_table = 2;
    const uint32_t exchange_tables = 1 * static_cast<uint32_t>(collectionCount<SQLTable>(exchange_table_lambda) > 1);
    const uint32_t alter_table
        = 6 * static_cast<uint32_t>(collectionHas<SQLTable>(alter_table_lambda) || collectionHas<SQLView>(attached_views));
    const uint32_t set_values = 5;
    const uint32_t attach = 2
        * static_cast<uint32_t>(collectionHas<SQLTable>(detached_tables) || collectionHas<SQLView>(detached_views)
                                || collectionHas<SQLDictionary>(detached_dictionaries)
                                || collectionHas<std::shared_ptr<SQLDatabase>>(detached_databases));
    const uint32_t detach = 2
        * static_cast<uint32_t>(collectionCount<SQLTable>(attached_tables) > 3 || collectionCount<SQLView>(attached_views) > 3
                                || collectionCount<SQLDictionary>(attached_dictionaries) > 3
                                || collectionCount<std::shared_ptr<SQLDatabase>>(attached_databases) > 3);
    const uint32_t create_database = 2 * static_cast<uint32_t>(static_cast<uint32_t>(databases.size()) < this->fc.max_databases);
    const uint32_t create_function = 5 * static_cast<uint32_t>(static_cast<uint32_t>(functions.size()) < this->fc.max_functions);
    const uint32_t system_stmt = 1;
    const uint32_t backup_or_restore = 1;
    const uint32_t create_dictionary = 10 * static_cast<uint32_t>(static_cast<uint32_t>(dictionaries.size()) < this->fc.max_dictionaries);
    const uint32_t select_query = 800;
    const uint32_t prob_space = create_table + create_view + drop + insert + light_delete + truncate + optimize_table + check_table
        + desc_table + exchange_tables + alter_table + set_values + attach + detach + create_database + create_function + system_stmt
        + backup_or_restore + create_dictionary + select_query;
    std::uniform_int_distribution<uint32_t> next_dist(1, prob_space);
    const uint32_t nopt = next_dist(rg.generator);

    chassert(this->ids.empty());
    if (create_table && nopt < (create_table + 1))
    {
        generateNextCreateTable(rg, sq->mutable_create_table());
    }
    else if (create_view && nopt < (create_table + create_view + 1))
    {
        generateNextCreateView(rg, sq->mutable_create_view());
    }
    else if (drop && nopt < (create_table + create_view + drop + 1))
    {
        generateNextDrop(rg, sq->mutable_drop());
    }
    else if (insert && nopt < (create_table + create_view + drop + insert + 1))
    {
        generateNextInsert(rg, sq->mutable_insert());
    }
    else if (light_delete && nopt < (create_table + create_view + drop + insert + light_delete + 1))
    {
        generateNextDelete(rg, sq->mutable_del());
    }
    else if (truncate && nopt < (create_table + create_view + drop + insert + light_delete + truncate + 1))
    {
        generateNextTruncate(rg, sq->mutable_trunc());
    }
    else if (optimize_table && nopt < (create_table + create_view + drop + insert + light_delete + truncate + optimize_table + 1))
    {
        generateNextOptimizeTable(rg, sq->mutable_opt());
    }
    else if (
        check_table && nopt < (create_table + create_view + drop + insert + light_delete + truncate + optimize_table + check_table + 1))
    {
        generateNextCheckTable(rg, sq->mutable_check());
    }
    else if (
        desc_table
        && nopt < (create_table + create_view + drop + insert + light_delete + truncate + optimize_table + check_table + desc_table + 1))
    {
        generateNextDescTable(rg, sq->mutable_desc());
    }
    else if (
        exchange_tables
        && nopt
            < (create_table + create_view + drop + insert + light_delete + truncate + optimize_table + check_table + desc_table
               + exchange_tables + 1))
    {
        generateNextExchangeTables(rg, sq->mutable_exchange());
    }
    else if (
        alter_table
        && nopt
            < (create_table + create_view + drop + insert + light_delete + truncate + optimize_table + check_table + desc_table
               + exchange_tables + alter_table + 1))
    {
        generateAlterTable(rg, sq->mutable_alter_table());
    }
    else if (
        set_values
        && nopt
            < (create_table + create_view + drop + insert + light_delete + truncate + optimize_table + check_table + desc_table
               + exchange_tables + alter_table + set_values + 1))
    {
        generateSettingValues(rg, serverSettings, sq->mutable_setting_values());
    }
    else if (
        attach
        && nopt
            < (create_table + create_view + drop + insert + light_delete + truncate + optimize_table + check_table + desc_table
               + exchange_tables + alter_table + set_values + attach + 1))
    {
        generateAttach(rg, sq->mutable_attach());
    }
    else if (
        detach
        && nopt
            < (create_table + create_view + drop + insert + light_delete + truncate + optimize_table + check_table + desc_table
               + exchange_tables + alter_table + set_values + attach + detach + 1))
    {
        generateDetach(rg, sq->mutable_detach());
    }
    else if (
        create_database
        && nopt
            < (create_table + create_view + drop + insert + light_delete + truncate + optimize_table + check_table + desc_table
               + exchange_tables + alter_table + set_values + attach + detach + create_database + 1))
    {
        generateNextCreateDatabase(rg, sq->mutable_create_database());
    }
    else if (
        create_function
        && nopt
            < (create_table + create_view + drop + insert + light_delete + truncate + optimize_table + check_table + desc_table
               + exchange_tables + alter_table + set_values + attach + detach + create_database + create_function + 1))
    {
        generateNextCreateFunction(rg, sq->mutable_create_function());
    }
    else if (
        system_stmt
        && nopt
            < (create_table + create_view + drop + insert + light_delete + truncate + optimize_table + check_table + desc_table
               + exchange_tables + alter_table + set_values + attach + detach + create_database + create_function + system_stmt + 1))
    {
        generateNextSystemStatement(rg, sq->mutable_system_cmd());
    }
    else if (
        backup_or_restore
        && nopt
            < (create_table + create_view + drop + insert + light_delete + truncate + optimize_table + check_table + desc_table
               + exchange_tables + alter_table + set_values + attach + detach + create_database + create_function + system_stmt
               + backup_or_restore + 1))
    {
        generateNextBackupOrRestore(rg, sq->mutable_backup_restore());
    }
    else if (
        create_dictionary
        && nopt
            < (create_table + create_view + drop + insert + light_delete + truncate + optimize_table + check_table + desc_table
               + exchange_tables + alter_table + set_values + attach + detach + create_database + create_function + system_stmt
               + backup_or_restore + create_dictionary + 1))
    {
        generateNextCreateDictionary(rg, sq->mutable_create_dictionary());
    }
    else if (
        select_query
        && nopt
            < (create_table + create_view + drop + insert + light_delete + truncate + optimize_table + check_table + desc_table
               + exchange_tables + alter_table + set_values + attach + detach + create_database + create_function + system_stmt
               + backup_or_restore + create_dictionary + select_query + 1))
    {
        generateTopSelect(rg, false, std::numeric_limits<uint32_t>::max(), sq->mutable_select());
    }
    else
    {
        chassert(0);
    }
}

struct ExplainOptValues
{
    const ExplainOption_ExplainOpt opt;
    const std::function<uint32_t(RandomGenerator &)> random_func;

    ExplainOptValues(const ExplainOption_ExplainOpt & e, const std::function<uint32_t(RandomGenerator &)> & rf)
        : opt(e)
        , random_func(rf)
    {
    }
};

static const std::function<uint32_t(RandomGenerator &)> trueOrFalseInt = [](RandomGenerator & rg) { return rg.nextBool() ? 1 : 0; };

static const std::vector<ExplainOptValues> explainSettings{
    ExplainOptValues(ExplainOption_ExplainOpt::ExplainOption_ExplainOpt_graph, trueOrFalseInt),
    ExplainOptValues(ExplainOption_ExplainOpt::ExplainOption_ExplainOpt_optimize, trueOrFalseInt),
    ExplainOptValues(ExplainOption_ExplainOpt::ExplainOption_ExplainOpt_oneline, trueOrFalseInt),
    ExplainOptValues(ExplainOption_ExplainOpt::ExplainOption_ExplainOpt_dump_ast, trueOrFalseInt),
    ExplainOptValues(ExplainOption_ExplainOpt::ExplainOption_ExplainOpt_dump_passes, trueOrFalseInt),
    ExplainOptValues(ExplainOption_ExplainOpt::ExplainOption_ExplainOpt_dump_tree, trueOrFalseInt),
    ExplainOptValues(ExplainOption_ExplainOpt::ExplainOption_ExplainOpt_run_passes, trueOrFalseInt),
    ExplainOptValues(
        ExplainOption_ExplainOpt::ExplainOption_ExplainOpt_passes, [](RandomGenerator & rg) { return rg.randomInt<uint32_t>(0, 32); }),
    ExplainOptValues(ExplainOption_ExplainOpt::ExplainOption_ExplainOpt_distributed, trueOrFalseInt),
    ExplainOptValues(ExplainOption_ExplainOpt::ExplainOption_ExplainOpt_sorting, trueOrFalseInt),
    ExplainOptValues(ExplainOption_ExplainOpt::ExplainOption_ExplainOpt_json, trueOrFalseInt),
    ExplainOptValues(ExplainOption_ExplainOpt::ExplainOption_ExplainOpt_description, trueOrFalseInt),
    ExplainOptValues(ExplainOption_ExplainOpt::ExplainOption_ExplainOpt_indexes, trueOrFalseInt),
    ExplainOptValues(ExplainOption_ExplainOpt::ExplainOption_ExplainOpt_keep_logical_steps, trueOrFalseInt),
    ExplainOptValues(ExplainOption_ExplainOpt::ExplainOption_ExplainOpt_actions, trueOrFalseInt),
    ExplainOptValues(ExplainOption_ExplainOpt::ExplainOption_ExplainOpt_header, trueOrFalseInt),
    ExplainOptValues(ExplainOption_ExplainOpt::ExplainOption_ExplainOpt_compact, trueOrFalseInt)};

void StatementGenerator::generateNextExplain(RandomGenerator & rg, ExplainQuery * eq)
{
    std::optional<ExplainQuery_ExplainValues> val;

    eq->set_is_explain(true);
    if (rg.nextSmallNumber() < 9)
    {
        val = std::optional<ExplainQuery_ExplainValues>(
            static_cast<ExplainQuery_ExplainValues>((rg.nextRandomUInt32() % static_cast<uint32_t>(ExplainQuery::ExplainValues_MAX)) + 1));

        eq->set_expl(val.value());
    }
    if (rg.nextBool())
    {
        chassert(this->ids.empty());
        if (val.has_value())
        {
            switch (val.value())
            {
                case ExplainQuery_ExplainValues::ExplainQuery_ExplainValues_AST:
                    this->ids.emplace_back(0);
                    this->ids.emplace_back(1);
                    break;
                case ExplainQuery_ExplainValues::ExplainQuery_ExplainValues_SYNTAX:
                    this->ids.emplace_back(2);
                    break;
                case ExplainQuery_ExplainValues::ExplainQuery_ExplainValues_QUERY_TREE:
                    this->ids.emplace_back(3);
                    this->ids.emplace_back(4);
                    this->ids.emplace_back(5);
                    this->ids.emplace_back(6);
                    this->ids.emplace_back(7);
                    break;
                case ExplainQuery_ExplainValues::ExplainQuery_ExplainValues_PLAN:
                case ExplainQuery_ExplainValues::ExplainQuery_ExplainValues_ESTIMATE:
                    this->ids.emplace_back(1);
                    this->ids.emplace_back(8);
                    this->ids.emplace_back(9);
                    this->ids.emplace_back(10);
                    this->ids.emplace_back(11);
                    this->ids.emplace_back(12);
                    this->ids.emplace_back(13);
                    this->ids.emplace_back(14);
                    this->ids.emplace_back(15);
                    break;
                case ExplainQuery_ExplainValues::ExplainQuery_ExplainValues_PIPELINE:
                    this->ids.emplace_back(0);
                    this->ids.emplace_back(15);
                    this->ids.emplace_back(16);
                    break;
                default:
                    break;
            }
        }
        else
        {
            this->ids.emplace_back(1);
            this->ids.emplace_back(9);
            this->ids.emplace_back(10);
            this->ids.emplace_back(11);
            this->ids.emplace_back(12);
            this->ids.emplace_back(13);
            this->ids.emplace_back(14);
            this->ids.emplace_back(15);
        }
        if (!this->ids.empty())
        {
            const size_t noptions = (static_cast<size_t>(rg.nextRandomUInt32()) % this->ids.size()) + 1;
            std::shuffle(ids.begin(), ids.end(), rg.generator);

            for (size_t i = 0; i < noptions; i++)
            {
                const auto & nopt = explainSettings[this->ids[i]];
                ExplainOption * eopt = eq->add_opts();

                eopt->set_opt(nopt.opt);
                eopt->set_val(nopt.random_func(rg));
            }
            this->ids.clear();
        }
    }
    generateNextQuery(rg, eq->mutable_inner_query());
}

void StatementGenerator::generateNextStatement(RandomGenerator & rg, SQLQuery & sq)
{
    const uint32_t start_transaction = 2 * static_cast<uint32_t>(!this->in_transaction);
    const uint32_t commit = 50 * static_cast<uint32_t>(this->in_transaction);
    const uint32_t explain_query = 10;
    const uint32_t run_query = 120;
    const uint32_t prob_space = start_transaction + commit + explain_query + run_query;
    std::uniform_int_distribution<uint32_t> next_dist(1, prob_space);
    const uint32_t nopt = next_dist(rg.generator);

    chassert(this->levels.empty());
    if (start_transaction && nopt < (start_transaction + 1))
    {
        sq.set_start_trans(true);
    }
    else if (commit && nopt < (start_transaction + commit + 1))
    {
        if (rg.nextSmallNumber() < 7)
        {
            sq.set_commit_trans(true);
        }
        else
        {
            sq.set_rollback_trans(true);
        }
    }
    else if (explain_query && nopt < (start_transaction + commit + explain_query + 1))
    {
        generateNextExplain(rg, sq.mutable_explain());
    }
    else if (run_query)
    {
        generateNextQuery(rg, sq.mutable_explain()->mutable_inner_query());
    }
    else
    {
        chassert(0);
    }
}

void StatementGenerator::dropTable(const bool staged, bool drop_peer, const uint32_t tname)
{
    auto & map_to_delete = staged ? this->staged_tables : this->tables;

    if (map_to_delete.find(tname) != map_to_delete.end())
    {
        if (drop_peer)
        {
            connections.dropPeerTableOnRemote(map_to_delete[tname]);
        }
        map_to_delete.erase(tname);
    }
}

void StatementGenerator::dropDatabase(const uint32_t dname)
{
    for (auto it = this->tables.cbegin(), next_it = it; it != this->tables.cend(); it = next_it)
    {
        ++next_it;
        if (it->second.db && it->second.db->dname == dname)
        {
            dropTable(false, true, it->first);
        }
    }
    for (auto it = this->views.cbegin(), next_it = it; it != this->views.cend(); it = next_it)
    {
        ++next_it;
        if (it->second.db && it->second.db->dname == dname)
        {
            this->views.erase(it);
        }
    }
    for (auto it = this->dictionaries.cbegin(), next_it = it; it != this->dictionaries.cend(); it = next_it)
    {
        ++next_it;
        if (it->second.db && it->second.db->dname == dname)
        {
            this->dictionaries.erase(it);
        }
    }
    this->databases.erase(dname);
}

void StatementGenerator::updateGenerator(const SQLQuery & sq, ExternalIntegrations & ei, bool success)
{
    const SQLQueryInner & query = sq.explain().inner_query();

    success &= (!ei.getRequiresExternalCallCheck() || ei.getNextExternalCallSucceeded());

    if (sq.has_explain() && query.has_create_table())
    {
        const uint32_t tname = static_cast<uint32_t>(std::stoul(query.create_table().est().table().table().substr(1)));

        if (!sq.explain().is_explain() && success)
        {
            if (query.create_table().create_opt() == CreateReplaceOption::Replace)
            {
                dropTable(false, true, tname);
            }
            this->tables[tname] = std::move(this->staged_tables[tname]);
        }
        dropTable(true, !success, tname);
    }
    else if (sq.has_explain() && query.has_create_view())
    {
        const uint32_t tname = static_cast<uint32_t>(std::stoul(query.create_view().est().table().table().substr(1)));

        if (!sq.explain().is_explain() && success)
        {
            if (query.create_view().create_opt() == CreateReplaceOption::Replace)
            {
                this->views.erase(tname);
            }
            this->views[tname] = std::move(this->staged_views[tname]);
        }
        this->staged_views.erase(tname);
    }
    else if (sq.has_explain() && query.has_create_dictionary())
    {
        const uint32_t dname = static_cast<uint32_t>(std::stoul(query.create_dictionary().est().table().table().substr(1)));

        if (!sq.explain().is_explain() && success)
        {
            if (query.create_view().create_opt() == CreateReplaceOption::Replace)
            {
                this->dictionaries.erase(dname);
            }
            this->dictionaries[dname] = std::move(this->staged_dictionaries[dname]);
        }
        this->staged_dictionaries.erase(dname);
    }
    else if (sq.has_explain() && !sq.explain().is_explain() && query.has_drop() && success)
    {
        const Drop & drp = query.drop();
        const bool istable = drp.object().has_est() && drp.object().est().table().table()[0] == 't';
        const bool isview = drp.object().has_est() && drp.object().est().table().table()[0] == 'v';
        const bool isdictionary = drp.object().has_est() && drp.object().est().table().table()[0] == 'd';
        const bool isdatabase = drp.object().has_database();
        const bool isfunction = drp.object().has_function();

        if (istable)
        {
            dropTable(false, true, static_cast<uint32_t>(std::stoul(drp.object().est().table().table().substr(1))));
        }
        else if (isview)
        {
            this->views.erase(static_cast<uint32_t>(std::stoul(drp.object().est().table().table().substr(1))));
        }
        else if (isdictionary)
        {
            this->dictionaries.erase(static_cast<uint32_t>(std::stoul(drp.object().est().table().table().substr(1))));
        }
        else if (isdatabase)
        {
            dropDatabase(static_cast<uint32_t>(std::stoul(drp.object().database().database().substr(1))));
        }
        else if (isfunction)
        {
            this->functions.erase(static_cast<uint32_t>(std::stoul(drp.object().function().function().substr(1))));
        }
    }
    else if (sq.has_explain() && !sq.explain().is_explain() && query.has_exchange() && success)
    {
        const uint32_t tname1 = static_cast<uint32_t>(std::stoul(query.exchange().est1().table().table().substr(1)));
        const uint32_t tname2 = static_cast<uint32_t>(std::stoul(query.exchange().est2().table().table().substr(1)));
        SQLTable tx = std::move(this->tables[tname1]);
        SQLTable ty = std::move(this->tables[tname2]);
        auto db_tmp = tx.db;

        tx.tname = tname2;
        tx.db = ty.db;
        ty.tname = tname1;
        ty.db = db_tmp;
        this->tables[tname2] = std::move(tx);
        this->tables[tname1] = std::move(ty);
    }
    else if (sq.has_explain() && !sq.explain().is_explain() && query.has_alter_table())
    {
        const AlterTable & at = query.alter_table();
        const bool isview = at.est().table().table()[0] == 'v';
        const uint32_t tname = static_cast<uint32_t>(std::stoul(at.est().table().table().substr(1)));

        if (isview)
        {
            SQLView & v = this->views[tname];

            for (int i = 0; i < at.other_alters_size() + 1; i++)
            {
                const AlterTableItem & ati = i == 0 ? at.alter() : at.other_alters(i - 1);

                if (success && ati.has_add_column() && !v.has_with_cols)
                {
                    v.cols.clear();
                    for (uint32_t j = 0; j < v.staged_ncols; j++)
                    {
                        v.cols.insert(j);
                    }
                }
            }
        }
        else
        {
            SQLTable & t = this->tables[tname];

            for (int i = 0; i < at.other_alters_size() + 1; i++)
            {
                const AlterTableItem & ati = i == 0 ? at.alter() : at.other_alters(i - 1);

                chassert(!ati.has_modify_query() && !ati.has_refresh());
                if (ati.has_add_column())
                {
                    const uint32_t cname = static_cast<uint32_t>(std::stoul(ati.add_column().new_col().col().column().substr(1)));

                    if (success)
                    {
                        t.cols[cname] = std::move(t.staged_cols[cname]);
                    }
                    t.staged_cols.erase(cname);
                }
                else if (ati.has_drop_column() && success)
                {
                    const ColumnPath & path = ati.drop_column();
                    const uint32_t cname = static_cast<uint32_t>(std::stoul(path.col().column().substr(1)));

                    if (path.sub_cols_size() == 0)
                    {
                        t.cols.erase(cname);
                    }
                    else
                    {
                        SQLColumn & col = t.cols.at(cname);
                        NestedType * ntp;

                        chassert(path.sub_cols_size() == 1);
                        if ((ntp = dynamic_cast<NestedType *>(col.tp)))
                        {
                            const uint32_t ncname = static_cast<uint32_t>(std::stoul(path.sub_cols(0).column().substr(1)));

                            for (auto it = ntp->subtypes.cbegin(), next_it = it; it != ntp->subtypes.cend(); it = next_it)
                            {
                                ++next_it;
                                if (it->cname == ncname)
                                {
                                    ntp->subtypes.erase(it);
                                    break;
                                }
                            }
                            if (ntp->subtypes.empty())
                            {
                                t.cols.erase(cname);
                            }
                        }
                    }
                }
                else if (ati.has_rename_column() && success)
                {
                    const ColumnPath & path = ati.rename_column().old_name();
                    const uint32_t old_cname = static_cast<uint32_t>(std::stoul(path.col().column().substr(1)));

                    if (path.sub_cols_size() == 0)
                    {
                        const uint32_t new_cname
                            = static_cast<uint32_t>(std::stoul(ati.rename_column().new_name().col().column().substr(1)));

                        t.cols[new_cname] = std::move(t.cols[old_cname]);
                        t.cols[new_cname].cname = new_cname;
                        t.cols.erase(old_cname);
                    }
                    else
                    {
                        SQLColumn & col = t.cols.at(old_cname);
                        NestedType * ntp;

                        chassert(path.sub_cols_size() == 1);
                        if ((ntp = dynamic_cast<NestedType *>(col.tp)))
                        {
                            const uint32_t nocname = static_cast<uint32_t>(std::stoul(path.sub_cols(0).column().substr(1)));

                            for (auto it = ntp->subtypes.begin(), next_it = it; it != ntp->subtypes.end(); it = next_it)
                            {
                                ++next_it;
                                if (it->cname == nocname)
                                {
                                    it->cname
                                        = static_cast<uint32_t>(std::stoul(ati.rename_column().new_name().sub_cols(0).column().substr(1)));
                                    break;
                                }
                            }
                        }
                    }
                }
                else if (ati.has_modify_column())
                {
                    const uint32_t cname = static_cast<uint32_t>(std::stoul(ati.modify_column().new_col().col().column().substr(1)));

                    if (success)
                    {
                        t.cols.erase(cname);
                        t.cols[cname] = std::move(t.staged_cols[cname]);
                    }
                    t.staged_cols.erase(cname);
                }
                else if (
                    ati.has_column_remove_property() && success
                    && ati.column_remove_property().property() < RemoveColumnProperty_ColumnProperties_CODEC)
                {
                    const ColumnPath & path = ati.column_remove_property().col();
                    const uint32_t cname = static_cast<uint32_t>(std::stoul(path.col().column().substr(1)));

                    if (path.sub_cols_size() == 0)
                    {
                        t.cols.at(cname).dmod = std::nullopt;
                    }
                }
                else if (ati.has_add_index())
                {
                    const uint32_t iname = static_cast<uint32_t>(std::stoul(ati.add_index().new_idx().idx().index().substr(1)));

                    if (success)
                    {
                        t.idxs[iname] = std::move(t.staged_idxs[iname]);
                    }
                    t.staged_idxs.erase(iname);
                }
                else if (ati.has_drop_index() && success)
                {
                    const uint32_t iname = static_cast<uint32_t>(std::stoul(ati.drop_index().index().substr(1)));

                    t.idxs.erase(iname);
                }
                else if (ati.has_add_projection())
                {
                    const uint32_t pname = static_cast<uint32_t>(std::stoul(ati.add_projection().proj().projection().substr(1)));

                    if (success)
                    {
                        t.projs.insert(pname);
                    }
                    t.staged_projs.erase(pname);
                }
                else if (ati.has_remove_projection() && success)
                {
                    const uint32_t pname = static_cast<uint32_t>(std::stoul(ati.remove_projection().projection().substr(1)));

                    t.projs.erase(pname);
                }
                else if (ati.has_add_constraint())
                {
                    const uint32_t pname = static_cast<uint32_t>(std::stoul(ati.add_constraint().constr().constraint().substr(1)));

                    if (success)
                    {
                        t.constrs.insert(pname);
                    }
                    t.staged_constrs.erase(pname);
                }
                else if (ati.has_remove_constraint() && success)
                {
                    const uint32_t pname = static_cast<uint32_t>(std::stoul(ati.remove_constraint().constraint().substr(1)));

                    t.constrs.erase(pname);
                }
                else if (ati.has_freeze_partition() && success)
                {
                    const FreezePartition & fp = ati.freeze_partition();

                    t.frozen_partitions[fp.fname()] = fp.has_single_partition() ? fp.single_partition().partition().partition_id() : "";
                }
                else if (ati.has_unfreeze_partition() && success)
                {
                    t.frozen_partitions.erase(ati.unfreeze_partition().fname());
                }
            }
        }
    }
    else if (sq.has_explain() && !sq.explain().is_explain() && query.has_attach() && success)
    {
        const Attach & att = query.attach();
        const bool istable = att.object().has_est() && att.object().est().table().table()[0] == 't';
        const bool isview = att.object().has_est() && att.object().est().table().table()[0] == 'v';
        const bool isdictionary = att.object().has_est() && att.object().est().table().table()[0] == 'd';
        const bool isdatabase = att.object().has_database();

        if (istable)
        {
            this->tables[static_cast<uint32_t>(std::stoul(att.object().est().table().table().substr(1)))].attached = DetachStatus::ATTACHED;
        }
        else if (isview)
        {
            this->views[static_cast<uint32_t>(std::stoul(att.object().est().table().table().substr(1)))].attached = DetachStatus::ATTACHED;
        }
        else if (isdictionary)
        {
            this->dictionaries[static_cast<uint32_t>(std::stoul(att.object().est().table().table().substr(1)))].attached
                = DetachStatus::ATTACHED;
        }
        else if (isdatabase)
        {
            const uint32_t dname = static_cast<uint32_t>(std::stoul(att.object().database().database().substr(1)));

            this->databases[dname]->attached = DetachStatus::ATTACHED;
            for (auto & [_, table] : this->tables)
            {
                if (table.db && table.db->dname == dname)
                {
                    table.attached = std::max(table.attached, DetachStatus::DETACHED);
                }
            }
        }
    }
    else if (sq.has_explain() && !sq.explain().is_explain() && query.has_detach() && success)
    {
        const Detach & det = query.detach();
        const bool istable = det.object().has_est() && det.object().est().table().table()[0] == 't';
        const bool isview = det.object().has_est() && det.object().est().table().table()[0] == 'v';
        const bool isdictionary = det.object().has_est() && det.object().est().table().table()[0] == 'd';
        const bool isdatabase = det.object().has_database();
        const bool is_permanent = det.permanently();

        if (istable)
        {
            this->tables[static_cast<uint32_t>(std::stoul(det.object().est().table().table().substr(1)))].attached
                = is_permanent ? DetachStatus::PERM_DETACHED : DetachStatus::DETACHED;
        }
        else if (isview)
        {
            this->views[static_cast<uint32_t>(std::stoul(det.object().est().table().table().substr(1)))].attached
                = is_permanent ? DetachStatus::PERM_DETACHED : DetachStatus::DETACHED;
        }
        else if (isdictionary)
        {
            this->dictionaries[static_cast<uint32_t>(std::stoul(det.object().est().table().table().substr(1)))].attached
                = is_permanent ? DetachStatus::PERM_DETACHED : DetachStatus::DETACHED;
        }
        else if (isdatabase)
        {
            const uint32_t dname = static_cast<uint32_t>(std::stoul(det.object().database().database().substr(1)));

            this->databases[dname]->attached = DetachStatus::DETACHED;
            for (auto & [_, table] : this->tables)
            {
                if (table.db && table.db->dname == dname)
                {
                    table.attached = std::max(table.attached, DetachStatus::DETACHED);
                }
            }
        }
    }
    else if (sq.has_explain() && query.has_create_database())
    {
        const uint32_t dname = static_cast<uint32_t>(std::stoul(query.create_database().database().database().substr(1)));

        if (!sq.explain().is_explain() && success)
        {
            this->databases[dname] = std::move(this->staged_databases[dname]);
        }
        this->staged_databases.erase(dname);
    }
    else if (sq.has_explain() && query.has_create_function())
    {
        const uint32_t fname = static_cast<uint32_t>(std::stoul(query.create_function().function().function().substr(1)));

        if (!sq.explain().is_explain() && success)
        {
            this->functions[fname] = std::move(this->staged_functions[fname]);
        }
        this->staged_functions.erase(fname);
    }
    else if (sq.has_explain() && !sq.explain().is_explain() && query.has_trunc() && query.trunc().has_database())
    {
        dropDatabase(static_cast<uint32_t>(std::stoul(query.trunc().database().database().substr(1))));
    }
    else if (sq.has_explain() && query.has_backup_restore() && !sq.explain().is_explain() && success)
    {
        const BackupRestore & br = query.backup_restore();
        const BackupRestoreElement & bre = br.backup_element();

        if (br.command() == BackupRestore_BackupCommand_BACKUP)
        {
            CatalogBackup newb;

            newb.backup_num = br.backup_number();
            newb.outf = br.out();
            if (br.has_format())
            {
                newb.out_format = br.format();
            }
            for (int i = 0; i < br.out_params_size(); i++)
            {
                newb.out_params.push_back(br.out_params(i));
            }
            if (bre.has_all_temporary())
            {
                for (auto & [key, value] : this->tables)
                {
                    if (value.is_temp)
                    {
                        newb.tables[key] = value;
                    }
                }
                newb.all_temporary = true;
            }
            else if (bre.has_all())
            {
                newb.tables = this->tables;
                newb.views = this->views;
                newb.databases = this->databases;
                newb.dictionaries = this->dictionaries;
                newb.everything = true;
            }
            else if (bre.has_bobject() && bre.bobject().sobject() == SQLObject::TABLE)
            {
                const BackupRestoreObject & bro = bre.bobject();

                if (!bro.object().est().has_database() || bro.object().est().database().database() != "system")
                {
                    const uint32_t tname = static_cast<uint32_t>(std::stoul(bro.object().est().table().table().substr(1)));

                    newb.tables[tname] = this->tables[tname];
                    if (bro.partitions_size())
                    {
                        newb.partition_id = bro.partitions(0).partition_id();
                    }
                }
                else
                {
                    newb.system_table = bro.object().est().table().table();
                }
            }
            else if (bre.has_bobject() && bre.bobject().sobject() == SQLObject::VIEW)
            {
                const uint32_t vname = static_cast<uint32_t>(std::stoul(bre.bobject().object().est().table().table().substr(1)));

                newb.views[vname] = this->views[vname];
            }
            else if (bre.has_bobject() && bre.bobject().sobject() == SQLObject::DICTIONARY)
            {
                const uint32_t dname = static_cast<uint32_t>(std::stoul(bre.bobject().object().est().table().table().substr(1)));

                newb.dictionaries[dname] = this->dictionaries[dname];
            }
            else if (bre.has_bobject() && bre.bobject().sobject() == SQLObject::DATABASE)
            {
                const uint32_t dname = static_cast<uint32_t>(std::stoul(bre.bobject().object().database().database().substr(1)));

                for (const auto & [key, val] : this->tables)
                {
                    if (val.db && val.db->dname == dname)
                    {
                        newb.tables[key] = val;
                    }
                }
                for (const auto & [key, val] : this->views)
                {
                    if (val.db && val.db->dname == dname)
                    {
                        newb.views[key] = val;
                    }
                }
                for (const auto & [key, val] : this->dictionaries)
                {
                    if (val.db && val.db->dname == dname)
                    {
                        newb.dictionaries[key] = val;
                    }
                }
                newb.databases[dname] = this->databases[dname];
            }
            this->backups[br.backup_number()] = std::move(newb);
        }
        else
        {
            const CatalogBackup & backup = backups.at(br.backup_number());

            if (!backup.partition_id.has_value())
            {
                for (const auto & [key, val] : backup.databases)
                {
                    this->databases[key] = val;
                }
                for (const auto & [key, val] : backup.tables)
                {
                    if (!val.db || this->databases.find(val.db->dname) != this->databases.end())
                    {
                        this->tables[key] = val;
                    }
                }
                for (const auto & [key, val] : backup.views)
                {
                    if (!val.db || this->databases.find(val.db->dname) != this->databases.end())
                    {
                        this->views[key] = val;
                    }
                }
                for (const auto & [key, val] : backup.dictionaries)
                {
                    if (!val.db || this->databases.find(val.db->dname) != this->databases.end())
                    {
                        this->dictionaries[key] = val;
                    }
                }
            }
        }
    }
    else if (sq.has_start_trans() && success)
    {
        this->in_transaction = true;
    }
    else if ((sq.has_commit_trans() || sq.has_rollback_trans()) && success)
    {
        this->in_transaction = false;
    }

    ei.resetExternalStatus();
}

}
