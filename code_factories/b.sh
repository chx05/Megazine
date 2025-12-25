gcc $1.c -std=c99 -g3 \
    -I/usr/lib/llvm-14/include -L/usr/lib/llvm-14/lib -lclang -lLLVM -lstdc++ \
    -Wall -Wextra -Wpedantic -Werror \
    -Wformat=2 -Wshadow -Wconversion -Wnull-dereference \
    -o $1.out