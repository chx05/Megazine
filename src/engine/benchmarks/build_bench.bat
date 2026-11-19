@g++ bench1.cpp -march=native -I. -L. -lntdll -lwinmm -pthread -std=c++20 -o bench1.exe -O3 -DNDEBUG
@g++ bench2a.cpp -march=native -I. -L. -lntdll -lwinmm -pthread -std=c++20 -o bench2a.exe -O3 -DNDEBUG
@g++ bench2b.cpp -march=native -I. -L. -lntdll -lwinmm -pthread -std=c++20 -o bench2b.exe -O3 -DNDEBUG

@g++ bench_user.cpp -x c ../../../playground_sqlite3/sqlite3.c -x none -I../../../playground_sqlite3 -I. -L. -static -lntdll -lwinmm -pthread -std=c++20 -O3 -DNDEBUG -march=native -DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DQS=0 -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_USE_ALLOCA -DSQLITE_MAX_MMAP_SIZE=17179869184 -o bench_user.exe