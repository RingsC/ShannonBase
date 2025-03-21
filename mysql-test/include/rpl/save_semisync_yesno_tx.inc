# ==== Purpose ====
#
# Save rpl_semi_sync_source_yes_tx's value to $rpl_semi_sync_source_yes_tx
# Save rpl_semi_sync_source_no_tx's value to $rpl_semi_sync_source_no_tx
#
# ==== Usage ====
#
# --source include/rpl/save_semisync_yesno_tx.inc
#

let $rpl_semi_sync_source_yes_tx=
    query_get_value(SHOW STATUS LIKE 'rpl_semi_sync_source_yes_tx', Value, 1);

let $rpl_semi_sync_source_no_tx=
    query_get_value(SHOW STATUS LIKE 'rpl_semi_sync_source_no_tx', Value, 1);
