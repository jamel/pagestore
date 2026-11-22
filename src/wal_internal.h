#pragma once

/* Recovery uses this internal accessor to check that the WAL belongs to the store. */

#include "pagestore/pagestore.h"
#include "pagestore/wal.h"

/* Returns the borrowed store pointer captured at WAL open. */
ps_store *ps_wal_store(const ps_wal *w);
