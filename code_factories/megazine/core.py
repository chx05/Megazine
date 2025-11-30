from clang.cindex import TranslationUnit
from fct import CppBuilder, CppPieceBuilder, ClangNode, collect_tagged_decls, piece, kindof, get_fully_qualified_name

class MegazineGenerator:
    def __init__(self, tu: TranslationUnit) -> None:
        self.structs: list[ClangNode] = collect_tagged_decls(tu.cursor, ["mgz"], ["struct_decl"])
        self.src: CppBuilder = CppBuilder(
            "mgz", auto_subnamespace=True,
            includes=["../../../lib/mgz.h"]
        )
    
    def gen_bound_load_store_for(self, fqn: str, struct: ClangNode) -> tuple[CppPieceBuilder, CppPieceBuilder]:
        store_pb = CppPieceBuilder()
        store_pb.line(f"template<> void store<{fqn}>(char const* db, {fqn} value)")
        store_pb.body()

        load_pb = CppPieceBuilder()
        load_pb.line(f"template<> {fqn} load<{fqn}>(char const* db)")
        load_pb.body()
        load_pb.add_indented("return {")

        for c in struct.get_children():
            if kindof(c) != "field_decl":
                continue

            load_pb.add_flat(f".{c.spelling}={c.type.get_size()},")
            #load_pb.line(f"// {c.type.spelling} {c.spelling}")
            #c.type.spelling
        
        load_pb.add_flat("};\n")
        
        store_pb.unbody()
        load_pb.unbody()
        return store_pb, load_pb

    def execute(self) -> None:
        self.src.add("unbound_load", piece(
"""
    template<typename T> void store(char const* db, T value) = delete;
    template<typename T> T load(char const* db) = delete;
"""
))

        for struct in self.structs:
            fqn = get_fully_qualified_name(struct)
            st, ld = self.gen_bound_load_store_for(fqn, struct)
            self.src.add(f"store<{fqn}>", st)
            self.src.add(f"load<{fqn}>", ld)
