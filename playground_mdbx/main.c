#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <mdbx.h>

#define SECS_IN_DAY   (86400LL)
#define SECS_IN_YEAR  (365LL * SECS_IN_DAY)

// --- Types & Schemas ---
#pragma pack(push, 1)
typedef struct {
    char name[21];       // UserName str(min: 1, max: 20, key)
    int64_t birth_day;   // Date (unix timestamp)
    uint64_t balance;    // Balance / MoneyAmmount
} User;

typedef struct {
    User user;
    int64_t suspension_time; // DateTime (unix timestamp)
} SuspendedUserVal;
#pragma pack(pop)

typedef struct {
    MDBX_env *env;
    MDBX_dbi all_users;        // Key: string (name) -> Val: User
    MDBX_dbi suspended_users;  // Key: uint64_t (id) -> Val: SuspendedUserVal
    MDBX_dbi meta;             // Key: string -> Val: uint64_t (next_id)
} Database;

// --- Helper: Sequence Generator ---
static uint64_t next_id(MDBX_txn *txn, MDBX_dbi meta_dbi) {
    uint64_t id = 1;
    MDBX_val k = { "seq_user_id", 11 }, v;
    if (mdbx_get(txn, meta_dbi, &k, &v) == MDBX_SUCCESS) {
        id = (*(uint64_t *)v.iov_base) + 1;
    }
    v.iov_base = &id; v.iov_len = sizeof(id);
    mdbx_put(txn, meta_dbi, &k, &v, 0);
    return id;
}

// --- Query 1: Transfer Money ---
bool transfer_money(Database *db, const char *src, const char *dst, uint64_t amount) {
    MDBX_txn *txn = NULL;
    mdbx_txn_begin(db->env, NULL, MDBX_TXN_READWRITE, &txn);

    MDBX_val k_src = { (void *)src, strlen(src) }, v_src;
    MDBX_val k_dst = { (void *)dst, strlen(dst) }, v_dst;

    if (mdbx_get(txn, db->all_users, &k_src, &v_src) != MDBX_SUCCESS ||
        mdbx_get(txn, db->all_users, &k_dst, &v_dst) != MDBX_SUCCESS) {
        mdbx_txn_abort(txn);
        return false; // User not found
    }

    User u_src = *(User *)v_src.iov_base;
    User u_dst = *(User *)v_dst.iov_base;

    if (u_src.balance < amount) {
        mdbx_txn_abort(txn); // source_has_not_enough_money
        return false;
    }

    u_src.balance -= amount;
    u_dst.balance += amount;

    v_src.iov_base = &u_src; v_src.iov_len = sizeof(User);
    v_dst.iov_base = &u_dst; v_dst.iov_len = sizeof(User);

    mdbx_put(txn, db->all_users, &k_src, &v_src, 0);
    mdbx_put(txn, db->all_users, &k_dst, &v_dst, 0);

    return mdbx_txn_commit(txn) == MDBX_SUCCESS;
}

// --- Query 2: Suspend User ---
bool suspend_user(Database *db, const char *name) {
    MDBX_txn *txn = NULL;
    mdbx_txn_begin(db->env, NULL, MDBX_TXN_READWRITE, &txn);

    MDBX_val k_name = { (void *)name, strlen(name) }, v_user;
    if (mdbx_get(txn, db->all_users, &k_name, &v_user) != MDBX_SUCCESS) {
        mdbx_txn_abort(txn);
        return false;
    }

    uint64_t id = next_id(txn, db->meta);
    SuspendedUserVal susp = {
        .user = *(User *)v_user.iov_base,
        .suspension_time = time(NULL)
    };

    MDBX_val k_id = { &id, sizeof(id) };
    MDBX_val v_susp = { &susp, sizeof(susp) };

    mdbx_put(txn, db->suspended_users, &k_id, &v_susp, 0);
    mdbx_del(txn, db->all_users, &k_name, NULL);

    return mdbx_txn_commit(txn) == MDBX_SUCCESS;
}

// --- Query 3: Task Delete Old Suspended Users (> 30 days) ---
void task_delete_old_suspended(Database *db) {
    MDBX_txn *txn = NULL;
    mdbx_txn_begin(db->env, NULL, MDBX_TXN_READWRITE, &txn);

    MDBX_cursor *cur = NULL;
    mdbx_cursor_open(txn, db->suspended_users, &cur);

    MDBX_val k, v;
    int64_t now = time(NULL);

    while (mdbx_cursor_get(cur, &k, &v, MDBX_NEXT) == MDBX_SUCCESS) {
        SuspendedUserVal *su = (SuspendedUserVal *)v.iov_base;
        if ((now - su->suspension_time) > (30 * SECS_IN_DAY)) {
            mdbx_cursor_del(cur, 0);
        }
    }

    mdbx_cursor_close(cur);
    mdbx_txn_commit(txn);
}

// --- Query 4: Names of Adult Users (>= 18 years) ---
void names_of_adult_users(Database *db) {
    MDBX_txn *txn = NULL;
    mdbx_txn_begin(db->env, NULL, MDBX_TXN_RDONLY, &txn);

    MDBX_cursor *cur = NULL;
    mdbx_cursor_open(txn, db->all_users, &cur);

    MDBX_val k, v;
    int64_t now = time(NULL);

    printf("--- Adult Users (>= 18) ---\n");
    while (mdbx_cursor_get(cur, &k, &v, MDBX_NEXT) == MDBX_SUCCESS) {
        User *u = (User *)v.iov_base;
        if ((now - u->birth_day) >= (18 * SECS_IN_YEAR)) {
            printf("- %.*s (Balance: %llu)\n", (int)k.iov_len, (char *)k.iov_base, (unsigned long long)u->balance);
        }
    }

    mdbx_cursor_close(cur);
    mdbx_txn_abort(txn);
}

// --- Helper: Insert User ---
void insert_user(Database *db, const char *name, int64_t birth_day, uint64_t balance) {
    MDBX_txn *txn = NULL;
    mdbx_txn_begin(db->env, NULL, MDBX_TXN_READWRITE, &txn);

    User u = { .birth_day = birth_day, .balance = balance };
    strncpy(u.name, name, sizeof(u.name) - 1);

    MDBX_val k = { (void *)name, strlen(name) };
    MDBX_val v = { &u, sizeof(User) };

    mdbx_put(txn, db->all_users, &k, &v, 0);
    mdbx_txn_commit(txn);
}

// --- Main Test Driver ---
int main(void) {
    mdbx_setup_debug(MDBX_LOG_WARN, 0, NULL);

    Database db;
    mdbx_env_create(&db.env);
    mdbx_env_set_maxdbs(db.env, 3);
    mdbx_env_open(db.env, "test_app.mdbx", MDBX_NOSUBDIR, 0644);

    // Initialize subdatabases
    MDBX_txn *txn = NULL;
    mdbx_txn_begin(db.env, NULL, MDBX_TXN_READWRITE, &txn);
    mdbx_dbi_open(txn, "all_users", MDBX_CREATE, &db.all_users);
    mdbx_dbi_open(txn, "suspended_users", MDBX_CREATE | MDBX_INTEGERKEY, &db.suspended_users);
    mdbx_dbi_open(txn, "meta", MDBX_CREATE, &db.meta);
    mdbx_txn_commit(txn);

    int64_t now = time(NULL);

    // Seed test users:
    // Alice: 25 years old, $500
    // Bob: 15 years old (minor), $100
    // Charlie: 30 years old, $50
    insert_user(&db, "Alice", now - (25 * SECS_IN_YEAR), 500);
    insert_user(&db, "Bob",   now - (15 * SECS_IN_YEAR), 100);
    insert_user(&db, "Charlie", now - (30 * SECS_IN_YEAR), 50);

    // Test 1: Query Adults
    names_of_adult_users(&db);

    // Test 2: Transfer Money (Alice -> Bob: $150)
    printf("\nTransferring 150 from Alice to Bob...\n");
    if (transfer_money(&db, "Alice", "Bob", 150)) {
        printf("Transfer successful!\n");
    }

    // Test 3: Failed Transfer (Charlie has only $50, tries to send $200)
    printf("Transferring 200 from Charlie to Alice...\n");
    if (!transfer_money(&db, "Charlie", "Alice", 200)) {
        printf("Transfer rejected: not enough money.\n");
    }

    // Test 4: Suspend Charlie
    printf("\nSuspending user Charlie...\n");
    suspend_user(&db, "Charlie");

    // Test 5: Verify Charlie is removed from active adult users
    names_of_adult_users(&db);

    // Test 6: Run Cleanup Task
    task_delete_old_suspended(&db);

    // Cleanup
    mdbx_env_close(db.env);
    return 0;
}