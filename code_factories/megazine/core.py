from clang.cindex import TranslationUnit
from fct import CppBuilder, CppPieceBuilder, ClangNode

class MegazineGenerator:
    def __init__(self, tu: TranslationUnit) -> None:
        self.prj: ClangNode = tu.cursor
        self.sources: list[CppBuilder] = []

        self.src: CppBuilder
    
    def close_src(self, new_src_name: str) -> None:
        self.sources.append(self.src)
        self.src = CppBuilder(new_src_name)
    
    def execute(self) -> None:
        pass
