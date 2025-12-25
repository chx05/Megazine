#include <stdio.h>
#include <string.h>
#include <clang-c/Index.h>

const char* TARGET_ANNOTATION = "Mgz_Root_Struct";

// Helper to convert CXString to C string and dispose it
void print_cxstring(CXString cx_str) {
    printf("%s", clang_getCString(cx_str));
    clang_disposeString(cx_str);
}

// Visitor to find the annotation inside a struct
enum CXChildVisitResult annotation_visitor(CXCursor cursor, CXCursor parent, CXClientData client_data) {
    (void)parent;
    if (clang_getCursorKind(cursor) == CXCursor_AnnotateAttr) {
        CXString annotation = clang_getCursorSpelling(cursor);
        const char* annotation_str = clang_getCString(annotation);

        if (strcmp(annotation_str, TARGET_ANNOTATION) == 0) {
            // Found it! Signal success to the caller
            int* found = (int*)client_data;
            *found = 1;
        }

        clang_disposeString(annotation);
    }
    return CXChildVisit_Continue;
}

// Main visitor to find structs
enum CXChildVisitResult root_visitor(CXCursor cursor, CXCursor parent, CXClientData client_data) {
    (void)parent;
    (void)client_data;
    // Only care about struct declarations
    if (clang_getCursorKind(cursor) == CXCursor_StructDecl) {
        int found_annotation = 0;

        // Visit the children of the struct to find the annotation attribute
        clang_visitChildren(cursor, annotation_visitor, &found_annotation);

        CXString name = clang_getCursorSpelling(cursor);
        char const* struct_name = clang_getCString(name);
        if (found_annotation)
            printf("Found annotated struct: %s\n", struct_name);
        else
            printf("Found non-annotated struct: %s\n", struct_name);
        
        clang_disposeString(name);
    }
    return CXChildVisit_Recurse;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: %s <file1.c> <file2.c> ...\n", argv[0]);
        return 1;
    }

    CXIndex index = clang_createIndex(0, 0);

    for (int i = 1; i < argc; i++) {
        printf("--- Parsing File: %s ---\n", argv[i]);

        char const* cmd_clang_args[1] = { "-DMGZ_LIBCLANG_ANALYSIS", };

        // Parse the source file
        CXTranslationUnit unit = clang_parseTranslationUnit(
            index, argv[i], cmd_clang_args, 1, NULL, 0, CXTranslationUnit_None);

        if (unit == NULL) {
            fprintf(stderr, "Unable to parse translation unit. Quitting.\n");
            continue;
        }

        // Get the root cursor and start visiting
        CXCursor cursor = clang_getTranslationUnitCursor(unit);
        clang_visitChildren(cursor, root_visitor, NULL);

        clang_disposeTranslationUnit(unit);
    }

    clang_disposeIndex(index);
    return 0;
}
