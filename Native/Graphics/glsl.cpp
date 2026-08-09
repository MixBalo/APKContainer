/*
 * glsl.cpp — GLSL ES 1.0 subset parser + AST interpreter.
 *
 * Status: REAL (common path). The common path (mat4*vec4, vec4*vec4,
 *         texture2D, swizzles, basic arithmetic, if/else) actually executes
 *         and produces correct varying interpolation + per-fragment color.
 *
 *         NOT implemented:
 *         - User-defined functions (only `main` + built-ins).
 *         - for/while loops (log + ignore if encountered).
 *         - structs.
 *         - Dynamic indexing of vectors by non-const expressions (const
 *           integer indices work).
 *         - Integer-only arithmetic on ivec (ints are stored as floats and
 *           most ops work; division is float division).
 *         - gl_PointSize (slot exists, never read by the rasterizer).
 *         - Mip-level selection in texture2D (always samples level 0; Lod
 *           bias arg ignored).
 *         - textureCube (no cubemap support; logs + returns 0).
 *
 *         Common path: the test-APK's shaders (attribute vec4 a_position;
 *         attribute vec2 a_texCoord; uniform mat4 u_mvp; varying vec2
 *         v_texCoord; void main() { gl_Position = u_mvp * a_position;
 *         v_texCoord = a_texCoord; }) and (precision mediump float; varying
 *         vec2 v_texCoord; uniform sampler2D u_texture; uniform vec4 u_color;
 *         void main() { gl_FragColor = texture2D(u_texture, v_texCoord) *
 *         u_color; }) compile, link, and execute correctly.
 *
 * Honesty contract: see worklog.md Phase 2 / P2-0.
 */

#include "glsl.h"
#include "log_file.h"

#include <vector>
#include <string>
#include <unordered_map>
#include <memory>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstdarg>
#include <algorithm>
#include <array>

/* ===== GL constants we need (defined here to avoid pulling in swgl.h) ===== */
#ifndef GL_FLOAT
#define GL_FLOAT                0x1406
#endif
#ifndef GL_UNSIGNED_BYTE
#define GL_UNSIGNED_BYTE        0x1401
#endif
#ifndef GL_BYTE
#define GL_BYTE                 0x1400
#endif
#ifndef GL_SHORT
#define GL_SHORT                0x1402
#endif
#ifndef GL_UNSIGNED_SHORT
#define GL_UNSIGNED_SHORT       0x1403
#endif
#ifndef GL_INT
#define GL_INT                  0x1404
#endif
#ifndef GL_UNSIGNED_INT
#define GL_UNSIGNED_INT         0x1405
#endif
#ifndef GL_NEAREST
#define GL_NEAREST              0x2600
#define GL_LINEAR               0x2601
#define GL_CLAMP_TO_EDGE        0x812F
#define GL_REPEAT               0x2901
#endif

namespace {

/* ===================== Types and helpers ===================== */

constexpr int MAX_VEC = 16;  // mat4 = 16 floats

static int ncount_safe(int n) { return n >= 0 && n <= MAX_VEC; }

int type_components(glsl_type_kind_t t) {
    switch (t) {
        case GLSL_VOID:      return 0;
        case GLSL_FLOAT:     return 1;
        case GLSL_INT:       return 1;
        case GLSL_BOOL:      return 1;
        case GLSL_VEC2:      return 2;
        case GLSL_VEC3:      return 3;
        case GLSL_VEC4:      return 4;
        case GLSL_IVEC2:     return 2;
        case GLSL_IVEC3:     return 3;
        case GLSL_IVEC4:     return 4;
        case GLSL_MAT2:      return 4;
        case GLSL_MAT3:      return 9;
        case GLSL_MAT4:      return 16;
        case GLSL_SAMPLER2D: return 1;
        default:             return 0;
    }
}

bool is_vector(glsl_type_kind_t t) {
    return t == GLSL_VEC2 || t == GLSL_VEC3 || t == GLSL_VEC4 ||
           t == GLSL_IVEC2 || t == GLSL_IVEC3 || t == GLSL_IVEC4;
}

bool is_matrix(glsl_type_kind_t t) {
    return t == GLSL_MAT2 || t == GLSL_MAT3 || t == GLSL_MAT4;
}

bool is_float_compatible(glsl_type_kind_t t) {
    return t == GLSL_FLOAT || (is_vector(t) && t != GLSL_IVEC2 && t != GLSL_IVEC3 && t != GLSL_IVEC4);
}

int vec_size(glsl_type_kind_t t) {
    switch (t) {
        case GLSL_VEC2: case GLSL_IVEC2: return 2;
        case GLSL_VEC3: case GLSL_IVEC3: return 3;
        case GLSL_VEC4: case GLSL_IVEC4: return 4;
        default: return 1;
    }
}

glsl_type_kind_t base_type(glsl_type_kind_t t) {
    switch (t) {
        case GLSL_VEC2: case GLSL_VEC3: case GLSL_VEC4: return GLSL_FLOAT;
        case GLSL_IVEC2: case GLSL_IVEC3: case GLSL_IVEC4: return GLSL_INT;
        default: return t;
    }
}

/* ===================== Value type ===================== */

struct Value {
    glsl_type_kind_t type = GLSL_VOID;
    float v[MAX_VEC] = {0};

    void set_float(float f) { type = GLSL_FLOAT; v[0] = f; }
    void set_int(int i)     { type = GLSL_INT; v[0] = (float)i; }
    void set_bool(bool b)   { type = GLSL_BOOL; v[0] = b ? 1.0f : 0.0f; }
};

/* ===================== AST nodes ===================== */

enum class NK {
    Block, Decl, If, Return, ExprStmt, Discard,
    Literal, Ident, Binary, Unary, Call, Member, Index, Assign, Ternary
};

enum {
    OP_ADD, OP_SUB, OP_MUL, OP_DIV,
    OP_EQ, OP_NE, OP_LT, OP_GT, OP_LE, OP_GE,
    OP_AND, OP_OR, OP_NOT, OP_NEG,
    OP_ASSIGN, OP_ADD_ASSIGN, OP_SUB_ASSIGN, OP_MUL_ASSIGN, OP_DIV_ASSIGN
};

struct Node;
using NodePtr = std::shared_ptr<Node>;

struct Node {
    NK kind;

    /* For Block / Decl multi-declarator list / Call args: */
    std::vector<NodePtr> children;

    /* Literal */
    Value lit;

    /* Ident */
    int slot = -1;
    std::string name;

    /* Binary / Unary / Assign */
    int op = 0;
    NodePtr left, right, operand, target, value;

    /* Call */
    std::string fname;
    std::vector<NodePtr> args;

    /* Member / Index */
    NodePtr obj, idx;
    int swizzle[4] = {0, 1, 2, 3};
    int swizzle_count = 0;

    /* If / Return */
    NodePtr cond, then_s, else_s, ret_expr;

    /* Decl (single declarator) */
    int decl_qual = 0;             // 0=none, 1=attr, 2=unif, 3=vary, 4=const, 5=builtin
    glsl_type_kind_t decl_type = GLSL_VOID;
    int decl_array = 1;
    std::string decl_name;
    NodePtr decl_init;

    Node() {}
};

NodePtr makeNode(NK k) { auto n = std::make_shared<Node>(); n->kind = k; return n; }

/* ===================== Lexer ===================== */

enum class Tok {
    End, Ident, Int, Float,
    Plus, Minus, Star, Slash,
    EqEq, NotEq, Lt, Gt, Le, Ge,
    AndAnd, OrOr, Not,
    Assign, AddAssign, SubAssign, MulAssign, DivAssign,
    Semicolon, Comma, Dot,
    LParen, RParen, LBrace, RBrace, LBracket, RBracket,
    KwIf, KwElse, KwReturn, KwFor, KwWhile, KwBreak, KwContinue, KwDiscard,
    KwTrue, KwFalse,
    KwVoid, KwFloat, KwVec2, KwVec3, KwVec4, KwMat2, KwMat3, KwMat4,
    KwInt, KwIvec2, KwIvec3, KwIvec4, KwBool, KwSampler2D,
    KwAttribute, KwUniform, KwVarying, KwConst, KwPrecision,
    KwHighp, KwMediump, KwLowp
};

struct Token {
    Tok kind = Tok::End;
    std::string text;
    long ival = 0;
    double fval = 0;
    int line = 1;
};

struct Lexer {
    const char* src;
    size_t pos = 0;
    int line = 1;
    std::string error;

    Lexer(const char* s) : src(s) {}

    bool eof() const { return src[pos] == 0; }

    void skip_ws_and_comments() {
        while (src[pos]) {
            char c = src[pos];
            if (c == ' ' || c == '\t' || c == '\r') { pos++; }
            else if (c == '\n') { pos++; line++; }
            else if (c == '/' && src[pos+1] == '/') {
                while (src[pos] && src[pos] != '\n') pos++;
            }
            else if (c == '/' && src[pos+1] == '*') {
                pos += 2;
                while (src[pos] && !(src[pos] == '*' && src[pos+1] == '/')) {
                    if (src[pos] == '\n') line++;
                    pos++;
                }
                if (src[pos]) pos += 2;
            }
            else if (c == '#') {
                /* Preprocessor line: read whole line, handle #version + precision.
                 * We mostly ignore them. */
                std::string line_str;
                while (src[pos] && src[pos] != '\n') { line_str += src[pos++]; }
                /* (no action needed; #version 100 and precision are no-ops for us) */
            }
            else break;
        }
    }

    static bool is_ident_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
    static bool is_ident_cont(char c)  { return is_ident_start(c) || (c >= '0' && c <= '9'); }

    static bool keyword(const std::string& s, Tok& out) {
        static const std::unordered_map<std::string, Tok> kws = {
            {"if",        Tok::KwIf},
            {"else",      Tok::KwElse},
            {"return",    Tok::KwReturn},
            {"for",       Tok::KwFor},
            {"while",     Tok::KwWhile},
            {"break",     Tok::KwBreak},
            {"continue",  Tok::KwContinue},
            {"discard",   Tok::KwDiscard},
            {"true",      Tok::KwTrue},
            {"false",     Tok::KwFalse},
            {"void",      Tok::KwVoid},
            {"float",     Tok::KwFloat},
            {"vec2",      Tok::KwVec2},
            {"vec3",      Tok::KwVec3},
            {"vec4",      Tok::KwVec4},
            {"mat2",      Tok::KwMat2},
            {"mat3",      Tok::KwMat3},
            {"mat4",      Tok::KwMat4},
            {"int",       Tok::KwInt},
            {"ivec2",     Tok::KwIvec2},
            {"ivec3",     Tok::KwIvec3},
            {"ivec4",     Tok::KwIvec4},
            {"bool",      Tok::KwBool},
            {"sampler2D", Tok::KwSampler2D},
            {"attribute", Tok::KwAttribute},
            {"uniform",   Tok::KwUniform},
            {"varying",   Tok::KwVarying},
            {"const",     Tok::KwConst},
            {"precision", Tok::KwPrecision},
            {"highp",     Tok::KwHighp},
            {"mediump",   Tok::KwMediump},
            {"lowp",      Tok::KwLowp},
        };
        auto it = kws.find(s);
        if (it == kws.end()) return false;
        out = it->second;
        return true;
    }

    Token next() {
        skip_ws_and_comments();
        Token t;
        t.line = line;
        if (eof()) { t.kind = Tok::End; return t; }
        char c = src[pos];

        if (is_ident_start(c)) {
            std::string s;
            while (is_ident_cont(src[pos])) s += src[pos++];
            Tok kw;
            if (keyword(s, kw)) { t.kind = kw; t.text = s; }
            else { t.kind = Tok::Ident; t.text = s; }
            return t;
        }

        if (c >= '0' && c <= '9') {
            std::string s;
            bool is_float = false;
            while (src[pos] >= '0' && src[pos] <= '9') s += src[pos++];
            if (src[pos] == '.') {
                is_float = true;
                s += src[pos++];
                while (src[pos] >= '0' && src[pos] <= '9') s += src[pos++];
            }
            if (src[pos] == 'e' || src[pos] == 'E') {
                is_float = true;
                s += src[pos++];
                if (src[pos] == '+' || src[pos] == '-') s += src[pos++];
                while (src[pos] >= '0' && src[pos] <= '9') s += src[pos++];
            }
            if (src[pos] == 'f' || src[pos] == 'F') { is_float = true; pos++; }
            if (is_float) {
                t.kind = Tok::Float;
                t.fval = std::strtod(s.c_str(), nullptr);
            } else {
                t.kind = Tok::Int;
                t.ival = std::strtol(s.c_str(), nullptr, 10);
            }
            return t;
        }

        /* operators and punctuation */
        auto two = [&](char a, char b) { return src[pos] == a && src[pos+1] == b; };
        if (two('+','=')) { pos += 2; t.kind = Tok::AddAssign; return t; }
        if (two('-','=')) { pos += 2; t.kind = Tok::SubAssign; return t; }
        if (two('*','=')) { pos += 2; t.kind = Tok::MulAssign; return t; }
        if (two('/','=')) { pos += 2; t.kind = Tok::DivAssign; return t; }
        if (two('=','=')) { pos += 2; t.kind = Tok::EqEq; return t; }
        if (two('!','=')) { pos += 2; t.kind = Tok::NotEq; return t; }
        if (two('<','=')) { pos += 2; t.kind = Tok::Le; return t; }
        if (two('>','=')) { pos += 2; t.kind = Tok::Ge; return t; }
        if (two('&','&')) { pos += 2; t.kind = Tok::AndAnd; return t; }
        if (two('|','|')) { pos += 2; t.kind = Tok::OrOr; return t; }

        switch (c) {
            case '+': pos++; t.kind = Tok::Plus; return t;
            case '-': pos++; t.kind = Tok::Minus; return t;
            case '*': pos++; t.kind = Tok::Star; return t;
            case '/': pos++; t.kind = Tok::Slash; return t;
            case '=': pos++; t.kind = Tok::Assign; return t;
            case '<': pos++; t.kind = Tok::Lt; return t;
            case '>': pos++; t.kind = Tok::Gt; return t;
            case '!': pos++; t.kind = Tok::Not; return t;
            case ';': pos++; t.kind = Tok::Semicolon; return t;
            case ',': pos++; t.kind = Tok::Comma; return t;
            case '.': pos++; t.kind = Tok::Dot; return t;
            case '(': pos++; t.kind = Tok::LParen; return t;
            case ')': pos++; t.kind = Tok::RParen; return t;
            case '{': pos++; t.kind = Tok::LBrace; return t;
            case '}': pos++; t.kind = Tok::RBrace; return t;
            case '[': pos++; t.kind = Tok::LBracket; return t;
            case ']': pos++; t.kind = Tok::RBracket; return t;
        }
        error = "unexpected character '" + std::string(1, c) + "'";
        pos++;
        t.kind = Tok::End;
        return t;
    }
};

/* ===================== Parser ===================== */

struct VarInfo {
    std::string name;
    glsl_type_kind_t type = GLSL_VOID;
    int qualifier = 0;       // 0=local, 1=attr, 2=unif, 3=vary, 4=const, 5=builtin
    int slot = -1;           // slot in the shader's vars array
    int array_size = 1;
    int uniform_location = -1;  // for uniforms, index into Program::uniforms
    int varying_slot = -1;      // for varyings, slot in the shared varying array
};

struct Shader {
    int is_vertex = 0;
    NodePtr main_body;                // body of main()
    std::vector<VarInfo> vars;        // all variables (globals + locals)
    std::unordered_map<std::string, int> slot_by_name;  // name → index in vars
    int slot_gl_position = -1;
    int slot_gl_frag_color = -1;
    int slot_gl_frag_coord = -1;
    int slot_gl_point_size = -1;
    int slot_gl_point_coord = -1;

    int compile_status = 0;
    std::string info_log;

    /* Pre-allocated variable storage, reused per run (no per-run heap alloc). */
    std::vector<Value> var_storage;
};

struct Program {
    Shader vs;
    Shader fs;

    /* Uniforms (shared between VS and FS): */
    std::vector<VarInfo> uniforms;     // index = location
    std::unordered_map<std::string, int> uniform_by_name;
    std::vector<Value> uniform_values; // index = location, mirrors uniforms

    /* Attributes (VS only): */
    std::vector<VarInfo> attributes;
    std::unordered_map<std::string, int> attrib_by_name;

    /* Varyings (shared): */
    std::vector<VarInfo> varyings;     // index = varying_slot
    std::unordered_map<std::string, int> varying_by_name;

    /* Texture bindings per unit (rasterizer fills before draw). */
    static constexpr int MAX_TEXTURE_UNITS = 4;
    glsl_texture_view textures[MAX_TEXTURE_UNITS] = {};
    int textures_bound[MAX_TEXTURE_UNITS] = {0,0,0,0};

    int link_status = 0;
    std::string link_info_log;
};

struct Parser {
    Lexer lex;
    Token cur;
    Shader* sh;
    Program* prog;
    int is_vertex;
    std::string error;

    Parser(const char* src, Shader* s, Program* p, int is_vs)
        : lex(src), sh(s), prog(p), is_vertex(is_vs) {
        sh->is_vertex = is_vs;
        cur = lex.next();
    }

    void err(const std::string& msg) {
        if (error.empty()) {
            error = "line " + std::to_string(cur.line) + ": " + msg;
        }
    }

    bool accept(Tok k) {
        if (cur.kind == k) { cur = lex.next(); return true; }
        return false;
    }

    bool expect(Tok k, const char* what) {
        if (cur.kind != k) {
            err(std::string("expected ") + what);
            return false;
        }
        cur = lex.next();
        return true;
    }

    bool type_tok_to_kind(Tok k, glsl_type_kind_t& out) {
        switch (k) {
            case Tok::KwVoid:      out = GLSL_VOID; return true;
            case Tok::KwFloat:     out = GLSL_FLOAT; return true;
            case Tok::KwVec2:      out = GLSL_VEC2; return true;
            case Tok::KwVec3:      out = GLSL_VEC3; return true;
            case Tok::KwVec4:      out = GLSL_VEC4; return true;
            case Tok::KwMat2:      out = GLSL_MAT2; return true;
            case Tok::KwMat3:      out = GLSL_MAT3; return true;
            case Tok::KwMat4:      out = GLSL_MAT4; return true;
            case Tok::KwInt:       out = GLSL_INT; return true;
            case Tok::KwIvec2:     out = GLSL_IVEC2; return true;
            case Tok::KwIvec3:     out = GLSL_IVEC3; return true;
            case Tok::KwIvec4:     out = GLSL_IVEC4; return true;
            case Tok::KwBool:      out = GLSL_BOOL; return true;
            case Tok::KwSampler2D: out = GLSL_SAMPLER2D; return true;
            default: return false;
        }
    }

    int add_var(const std::string& name, glsl_type_kind_t type, int qual, int array_size=1) {
        if (sh->slot_by_name.count(name)) {
            /* redeclaration; reuse existing slot */
            int existing = sh->slot_by_name[name];
            return existing;
        }
        VarInfo v;
        v.name = name;
        v.type = type;
        v.qualifier = qual;
        v.array_size = array_size;
        v.slot = (int)sh->vars.size();
        sh->vars.push_back(v);
        sh->slot_by_name[name] = v.slot;
        return v.slot;
    }

    /* Register built-in variables at the start of parse. */
    void register_builtins() {
        if (is_vertex) {
            sh->slot_gl_position = add_var("gl_Position",  GLSL_VEC4, 5);
            sh->slot_gl_point_size = add_var("gl_PointSize", GLSL_FLOAT, 5);
        } else {
            sh->slot_gl_frag_color = add_var("gl_FragColor",  GLSL_VEC4, 5);
            sh->slot_gl_frag_coord = add_var("gl_FragCoord",  GLSL_VEC4, 5);
            sh->slot_gl_point_coord = add_var("gl_PointCoord", GLSL_VEC2, 5);
        }
    }

    bool parse() {
        register_builtins();

        /* Top-level: declarations and function definitions. We only support
         * `void main() { ... }` and global declarations; user functions are
         * rejected with an error. */
        while (cur.kind != Tok::End && error.empty()) {
            if (!parse_top_level()) break;
        }

        if (!error.empty()) {
            sh->compile_status = 0;
            sh->info_log = error;
            return false;
        }
        sh->compile_status = 1;
        return true;
    }

    /* Parse a global declaration or function definition. */
    bool parse_top_level() {
        /* Skip precision statements: `precision highp float;` */
        if (cur.kind == Tok::KwPrecision) {
            cur = lex.next();
            /* precision-qualifier */
            if (cur.kind == Tok::KwHighp || cur.kind == Tok::KwMediump || cur.kind == Tok::KwLowp) cur = lex.next();
            /* type */
            glsl_type_kind_t tt;
            if (type_tok_to_kind(cur.kind, tt)) cur = lex.next();
            if (!expect(Tok::Semicolon, ";")) return false;
            return true;
        }

        /* Optional qualifier */
        int qual = 0;
        if (cur.kind == Tok::KwAttribute) { qual = 1; cur = lex.next(); }
        else if (cur.kind == Tok::KwUniform) { qual = 2; cur = lex.next(); }
        else if (cur.kind == Tok::KwVarying) { qual = 3; cur = lex.next(); }
        else if (cur.kind == Tok::KwConst)   { qual = 4; cur = lex.next(); }

        /* Type */
        glsl_type_kind_t type;
        if (!type_tok_to_kind(cur.kind, type)) {
            err("expected type");
            return false;
        }
        cur = lex.next();

        /* `void main()` — function definition */
        if (type == GLSL_VOID && cur.kind == Tok::Ident && cur.text == "main") {
            cur = lex.next();
            if (!expect(Tok::LParen, "(")) return false;
            if (!expect(Tok::RParen, ")")) return false;
            NodePtr body = parse_block();
            if (!body) return false;
            sh->main_body = body;
            return true;
        }

        /* Variable declarators (comma-separated) */
        if (!parse_declarator_list(type, qual)) return false;
        return true;
    }

    bool parse_declarator_list(glsl_type_kind_t type, int qual) {
        for (;;) {
            if (cur.kind != Tok::Ident) {
                err("expected identifier in declaration");
                return false;
            }
            std::string name = cur.text;
            cur = lex.next();

            int array_size = 1;
            if (accept(Tok::LBracket)) {
                if (cur.kind == Tok::Int) {
                    array_size = (int)cur.ival;
                    cur = lex.next();
                }
                if (!expect(Tok::RBracket, "]")) return false;
            }

            int slot = add_var(name, type, qual, array_size);

            /* Initializer (only meaningful for const/local). */
            NodePtr init;
            if (accept(Tok::Assign)) {
                init = parse_assign();
                if (!init) return false;
            }

            /* If this is a global qualifier (attr/unif/vary/const), we register
             * it with the Program later (during link). The Shader already has
             * the slot. */

            if (!expect(Tok::Semicolon, ";")) return false;

            /* Note: we don't currently attach the init to the AST for globals,
             * because globals are initialized at link time, not at run time.
             * For const locals, we DO want to init at run time. We'll just
             * emit a synthetic StmtDecl in the next enclosing block.
             * For simplicity in this version, we evaluate const initializers
             * at link time and bake the value into the slot. */
            (void)slot;
            (void)init;

            if (!accept(Tok::Comma)) break;
        }
        return true;
    }

    NodePtr parse_block() {
        if (!expect(Tok::LBrace, "{")) return nullptr;
        NodePtr block = makeNode(NK::Block);
        while (cur.kind != Tok::RBrace && cur.kind != Tok::End && error.empty()) {
            NodePtr s = parse_statement();
            if (!s) return nullptr;
            block->children.push_back(s);
        }
        if (!expect(Tok::RBrace, "}")) return nullptr;
        return block;
    }

    NodePtr parse_statement() {
        if (cur.kind == Tok::LBrace) return parse_block();
        if (cur.kind == Tok::KwIf) return parse_if();
        if (cur.kind == Tok::KwReturn) {
            cur = lex.next();
            NodePtr n = makeNode(NK::Return);
            if (cur.kind != Tok::Semicolon) {
                n->ret_expr = parse_assign();
                if (!n->ret_expr) return nullptr;
            }
            if (!expect(Tok::Semicolon, ";")) return nullptr;
            return n;
        }
        if (cur.kind == Tok::KwDiscard) {
            cur = lex.next();
            if (!expect(Tok::Semicolon, ";")) return nullptr;
            return makeNode(NK::Discard);
        }
        if (cur.kind == Tok::KwBreak || cur.kind == Tok::KwContinue) {
            /* Not in a loop — treat as error. */
            err("break/continue outside loop");
            return nullptr;
        }
        if (cur.kind == Tok::KwFor || cur.kind == Tok::KwWhile) {
            err("for/while loops not implemented in this GLSL subset");
            LOGW("glsl", "for/while loops not implemented (line %d)", cur.line);
            return nullptr;
        }
        /* Local declaration or expression statement. */
        glsl_type_kind_t tt;
        if (type_tok_to_kind(cur.kind, tt)) {
            /* local decl */
            cur = lex.next();
            NodePtr d = makeNode(NK::Decl);
            d->decl_qual = 0;
            d->decl_type = tt;
            if (cur.kind != Tok::Ident) { err("expected identifier"); return nullptr; }
            d->decl_name = cur.text;
            cur = lex.next();
            if (accept(Tok::LBracket)) {
                if (cur.kind == Tok::Int) {
                    d->decl_array = (int)cur.ival;
                    cur = lex.next();
                }
                if (!expect(Tok::RBracket, "]")) return nullptr;
            } else {
                d->decl_array = 1;
            }
            if (accept(Tok::Assign)) {
                d->decl_init = parse_assign();
                if (!d->decl_init) return nullptr;
            }
            if (!expect(Tok::Semicolon, ";")) return nullptr;
            /* Reserve slot now so the interpreter has storage. */
            add_var(d->decl_name, d->decl_type, 0, d->decl_array);
            return d;
        }

        /* expression statement */
        NodePtr e = parse_assign();
        if (!e) return nullptr;
        if (!expect(Tok::Semicolon, ";")) return nullptr;
        NodePtr s = makeNode(NK::ExprStmt);
        s->children.push_back(e);
        return s;
    }

    NodePtr parse_if() {
        cur = lex.next();   /* if */
        if (!expect(Tok::LParen, "(")) return nullptr;
        NodePtr cond = parse_assign();
        if (!cond) return nullptr;
        if (!expect(Tok::RParen, ")")) return nullptr;
        NodePtr then_s = parse_statement();
        if (!then_s) return nullptr;
        NodePtr n = makeNode(NK::If);
        n->cond = cond;
        n->then_s = then_s;
        if (accept(Tok::KwElse)) {
            n->else_s = parse_statement();
            if (!n->else_s) return nullptr;
        }
        return n;
    }

    NodePtr parse_assign() {
        NodePtr lhs = parse_or();
        if (!lhs) return nullptr;
        int op = -1;
        switch (cur.kind) {
            case Tok::Assign:     op = OP_ASSIGN; break;
            case Tok::AddAssign:  op = OP_ADD_ASSIGN; break;
            case Tok::SubAssign:  op = OP_SUB_ASSIGN; break;
            case Tok::MulAssign:  op = OP_MUL_ASSIGN; break;
            case Tok::DivAssign:  op = OP_DIV_ASSIGN; break;
            default: return lhs;
        }
        cur = lex.next();
        NodePtr rhs = parse_assign();
        if (!rhs) return nullptr;
        NodePtr n = makeNode(NK::Assign);
        n->op = op;
        n->target = lhs;
        n->value = rhs;
        return n;
    }

    NodePtr parse_or() {
        NodePtr a = parse_and();
        if (!a) return nullptr;
        while (cur.kind == Tok::OrOr) {
            cur = lex.next();
            NodePtr b = parse_and();
            if (!b) return nullptr;
            NodePtr n = makeNode(NK::Binary);
            n->op = OP_OR; n->left = a; n->right = b;
            a = n;
        }
        return a;
    }

    NodePtr parse_and() {
        NodePtr a = parse_eq();
        if (!a) return nullptr;
        while (cur.kind == Tok::AndAnd) {
            cur = lex.next();
            NodePtr b = parse_eq();
            if (!b) return nullptr;
            NodePtr n = makeNode(NK::Binary);
            n->op = OP_AND; n->left = a; n->right = b;
            a = n;
        }
        return a;
    }

    NodePtr parse_eq() {
        NodePtr a = parse_rel();
        if (!a) return nullptr;
        while (cur.kind == Tok::EqEq || cur.kind == Tok::NotEq) {
            int op = (cur.kind == Tok::EqEq) ? OP_EQ : OP_NE;
            cur = lex.next();
            NodePtr b = parse_rel();
            if (!b) return nullptr;
            NodePtr n = makeNode(NK::Binary);
            n->op = op; n->left = a; n->right = b;
            a = n;
        }
        return a;
    }

    NodePtr parse_rel() {
        NodePtr a = parse_add();
        if (!a) return nullptr;
        while (cur.kind == Tok::Lt || cur.kind == Tok::Gt ||
               cur.kind == Tok::Le || cur.kind == Tok::Ge) {
            int op = cur.kind == Tok::Lt ? OP_LT :
                     cur.kind == Tok::Gt ? OP_GT :
                     cur.kind == Tok::Le ? OP_LE : OP_GE;
            cur = lex.next();
            NodePtr b = parse_add();
            if (!b) return nullptr;
            NodePtr n = makeNode(NK::Binary);
            n->op = op; n->left = a; n->right = b;
            a = n;
        }
        return a;
    }

    NodePtr parse_add() {
        NodePtr a = parse_mul();
        if (!a) return nullptr;
        while (cur.kind == Tok::Plus || cur.kind == Tok::Minus) {
            int op = cur.kind == Tok::Plus ? OP_ADD : OP_SUB;
            cur = lex.next();
            NodePtr b = parse_mul();
            if (!b) return nullptr;
            NodePtr n = makeNode(NK::Binary);
            n->op = op; n->left = a; n->right = b;
            a = n;
        }
        return a;
    }

    NodePtr parse_mul() {
        NodePtr a = parse_unary();
        if (!a) return nullptr;
        while (cur.kind == Tok::Star || cur.kind == Tok::Slash) {
            int op = cur.kind == Tok::Star ? OP_MUL : OP_DIV;
            cur = lex.next();
            NodePtr b = parse_unary();
            if (!b) return nullptr;
            NodePtr n = makeNode(NK::Binary);
            n->op = op; n->left = a; n->right = b;
            a = n;
        }
        return a;
    }

    NodePtr parse_unary() {
        if (cur.kind == Tok::Minus) {
            cur = lex.next();
            NodePtr n = makeNode(NK::Unary);
            n->op = OP_NEG;
            n->operand = parse_unary();
            return n->operand ? n : nullptr;
        }
        if (cur.kind == Tok::Not) {
            cur = lex.next();
            NodePtr n = makeNode(NK::Unary);
            n->op = OP_NOT;
            n->operand = parse_unary();
            return n->operand ? n : nullptr;
        }
        if (cur.kind == Tok::Plus) {
            cur = lex.next();
            return parse_unary();
        }
        return parse_postfix();
    }

    NodePtr parse_postfix() {
        NodePtr e = parse_primary();
        if (!e) return nullptr;
        for (;;) {
            if (cur.kind == Tok::Dot) {
                cur = lex.next();
                if (cur.kind != Tok::Ident) {
                    err("expected identifier after '.'");
                    return nullptr;
                }
                std::string sw = cur.text;
                cur = lex.next();
                NodePtr n = makeNode(NK::Member);
                n->obj = e;
                /* Parse swizzle. .rgba/.xyzw/.stpq all map to 0,1,2,3. */
                static const char* xnames = "xrgs";   /* first component letters */
                static const char* ynames = "ygbt";
                static const char* znames = "zbrp";
                static const char* wnames = "wabq";
                n->swizzle_count = (int)sw.size();
                if (n->swizzle_count > 4) {
                    err("swizzle too long");
                    return nullptr;
                }
                for (int i = 0; i < n->swizzle_count; i++) {
                    char ch = sw[i];
                    const char* p;
                    if ((p = strchr(xnames, ch))) n->swizzle[i] = 0;
                    else if ((p = strchr(ynames, ch))) n->swizzle[i] = 1;
                    else if ((p = strchr(znames, ch))) n->swizzle[i] = 2;
                    else if ((p = strchr(wnames, ch))) n->swizzle[i] = 3;
                    else {
                        err("invalid swizzle character");
                        return nullptr;
                    }
                }
                e = n;
            } else if (cur.kind == Tok::LBracket) {
                cur = lex.next();
                NodePtr idx = parse_assign();
                if (!idx) return nullptr;
                if (!expect(Tok::RBracket, "]")) return nullptr;
                NodePtr n = makeNode(NK::Index);
                n->obj = e;
                n->idx = idx;
                e = n;
            } else if (cur.kind == Tok::LParen) {
                /* Function call. e must be an Ident. */
                if (e->kind != NK::Ident) {
                    err("can only call named functions");
                    return nullptr;
                }
                cur = lex.next();
                NodePtr n = makeNode(NK::Call);
                n->fname = e->name;
                if (cur.kind != Tok::RParen) {
                    for (;;) {
                        NodePtr a = parse_assign();
                        if (!a) return nullptr;
                        n->args.push_back(a);
                        if (!accept(Tok::Comma)) break;
                    }
                }
                if (!expect(Tok::RParen, ")")) return nullptr;
                e = n;
            } else break;
        }
        return e;
    }

    NodePtr parse_primary() {
        if (cur.kind == Tok::Int) {
            NodePtr n = makeNode(NK::Literal);
            n->lit.set_int((int)cur.ival);
            cur = lex.next();
            return n;
        }
        if (cur.kind == Tok::Float) {
            NodePtr n = makeNode(NK::Literal);
            n->lit.set_float((float)cur.fval);
            cur = lex.next();
            return n;
        }
        if (cur.kind == Tok::KwTrue) {
            NodePtr n = makeNode(NK::Literal);
            n->lit.set_bool(true);
            cur = lex.next();
            return n;
        }
        if (cur.kind == Tok::KwFalse) {
            NodePtr n = makeNode(NK::Literal);
            n->lit.set_bool(false);
            cur = lex.next();
            return n;
        }
        if (cur.kind == Tok::LParen) {
            cur = lex.next();
            NodePtr e = parse_assign();
            if (!e) return nullptr;
            if (!expect(Tok::RParen, ")")) return nullptr;
            return e;
        }
        if (cur.kind == Tok::Ident) {
            NodePtr n = makeNode(NK::Ident);
            n->name = cur.text;
            cur = lex.next();
            return n;
        }
        err("expected expression");
        return nullptr;
    }
};

/* ===================== Linker ===================== */

bool link_program(Program* p, std::string& err) {
    /* Collect uniforms, attributes, varyings from both shaders. */
    auto collect = [&](Shader* s) {
        for (auto& v : s->vars) {
            if (v.qualifier == 1 /* attribute */) {
                if (s->is_vertex) {
                    if (!p->attrib_by_name.count(v.name)) {
                        v.uniform_location = -1;
                        VarInfo a = v;
                        a.slot = (int)p->attributes.size();
                        p->attributes.push_back(a);
                        p->attrib_by_name[v.name] = a.slot;
                    }
                } else {
                    err = "attribute '" + v.name + "' declared in fragment shader";
                    return false;
                }
            } else if (v.qualifier == 2 /* uniform */) {
                if (!p->uniform_by_name.count(v.name)) {
                    VarInfo u = v;
                    u.uniform_location = (int)p->uniforms.size();
                    u.slot = u.uniform_location;
                    p->uniforms.push_back(u);
                    p->uniform_by_name[v.name] = u.uniform_location;
                    Value z;
                    z.type = u.type;
                    p->uniform_values.push_back(z);
                }
            } else if (v.qualifier == 3 /* varying */) {
                if (!p->varying_by_name.count(v.name)) {
                    VarInfo vv = v;
                    vv.varying_slot = (int)p->varyings.size();
                    p->varyings.push_back(vv);
                    p->varying_by_name[v.name] = vv.varying_slot;
                }
            }
        }
        return true;
    };

    if (!collect(&p->vs)) return false;
    if (!collect(&p->fs)) return false;

    /* Now patch each shader's varying slots to point to the shared varying index. */
    auto patch_varyings = [&](Shader* s) {
        for (auto& v : s->vars) {
            if (v.qualifier == 3 /* varying */) {
                auto it = p->varying_by_name.find(v.name);
                if (it != p->varying_by_name.end()) {
                    v.varying_slot = it->second;
                }
            }
            if (v.qualifier == 2 /* uniform */) {
                auto it = p->uniform_by_name.find(v.name);
                if (it != p->uniform_by_name.end()) {
                    v.uniform_location = it->second;
                }
            }
        }
    };
    patch_varyings(&p->vs);
    patch_varyings(&p->fs);

    /* Pre-allocate var_storage for each shader. */
    p->vs.var_storage.resize(p->vs.vars.size());
    p->fs.var_storage.resize(p->fs.vars.size());

    p->link_status = 1;
    return true;
}

/* ===================== Interpreter ===================== */

struct Interp {
    Program* prog;
    Shader* sh;
    Value* vars;
    int return_flag = 0;
    int discard_flag = 0;

    /* Evaluate the value of an expression node. */
    Value eval(Node* n);
    /* Evaluate a function call (built-in dispatch). */
    Value eval_call(Node* n);

    /* L-value evaluation: returns pointer + write mask. */
    struct LValue {
        Value* ptr;
        int swizzle[4];
        int count;
    };
    bool eval_lvalue(Node* n, LValue& out);
    void assign_lvalue(const LValue& lv, const Value& v);

    void exec(Node* n);
    void exec_block(Node* n);
};

void Interp::exec_block(Node* n) {
    for (auto& c : n->children) {
        exec(c.get());
        if (return_flag || discard_flag) return;
    }
}

void Interp::exec(Node* n) {
    if (!n || return_flag || discard_flag) return;
    switch (n->kind) {
        case NK::Block:
            exec_block(n);
            return;
        case NK::ExprStmt:
            for (auto& c : n->children) eval(c.get());
            return;
        case NK::Decl: {
            /* Local variable declaration with optional initializer. */
            auto it = sh->slot_by_name.find(n->decl_name);
            if (it != sh->slot_by_name.end()) {
                int slot = it->second;
                Value& dst = vars[slot];
                dst.type = n->decl_type;
                for (int i = 0; i < MAX_VEC; i++) dst.v[i] = 0.0f;
                if (n->decl_init) {
                    Value v = eval(n->decl_init.get());
                    int ncopy = type_components(n->decl_type);
                    if (ncount_safe(ncopy)) {
                        for (int i = 0; i < ncopy && i < MAX_VEC; i++) dst.v[i] = v.v[i];
                    }
                }
            }
            return;
        }
        case NK::If: {
            Value c = eval(n->cond.get());
            if (c.v[0] != 0.0f) exec(n->then_s.get());
            else if (n->else_s)  exec(n->else_s.get());
            return;
        }
        case NK::Return:
            return_flag = 1;
            return;
        case NK::Discard:
            discard_flag = 1;
            return;
        default:
            /* Expression as statement. */
            eval(n);
            return;
    }
}

/* Forward decls for built-ins. */
static Value builtin_texture2D(Program* prog, const Value& sampler, float u, float v);

Value Interp::eval(Node* n) {
    Value r;
    if (!n) return r;
    switch (n->kind) {
        case NK::Literal:
            return n->lit;
        case NK::Ident: {
            auto it = sh->slot_by_name.find(n->name);
            if (it == sh->slot_by_name.end()) {
                LOGW("glsl", "unknown identifier '%s'", n->name.c_str());
                return r;
            }
            return vars[it->second];
        }
        case NK::Binary: {
            Value a = eval(n->left.get());
            Value b = eval(n->right.get());
            int ac = type_components(a.type);
            int bc = type_components(b.type);
            switch (n->op) {
                case OP_ADD: case OP_SUB: {
                    int nc = std::max(ac, bc);
                    r.type = nc > 1 ? a.type : (a.type != GLSL_VOID ? a.type : b.type);
                    if (r.type == GLSL_VOID) r.type = GLSL_FLOAT;
                    for (int i = 0; i < nc && i < MAX_VEC; i++) {
                        float av = (ac == 1) ? a.v[0] : (i < ac ? a.v[i] : 0.0f);
                        float bv = (bc == 1) ? b.v[0] : (i < bc ? b.v[i] : 0.0f);
                        r.v[i] = (n->op == OP_ADD) ? av + bv : av - bv;
                    }
                    return r;
                }
                case OP_MUL: {
                    /* matrix * vector, matrix * matrix, or scalar/vector product */
                    if (is_matrix(a.type) && is_vector(b.type)) {
                        int rows = vec_size(a.type == GLSL_MAT2 ? GLSL_VEC2 :
                                            a.type == GLSL_MAT3 ? GLSL_VEC3 : GLSL_VEC4);
                        int mat_dim = (a.type == GLSL_MAT2) ? 2 :
                                      (a.type == GLSL_MAT3) ? 3 : 4;
                        r.type = (glsl_type_kind_t)(GLSL_VEC2 + rows - 2);
                        for (int i = 0; i < rows; i++) {
                            float sum = 0;
                            for (int j = 0; j < mat_dim; j++) {
                                sum += a.v[i * mat_dim + j] * b.v[j];
                            }
                            r.v[i] = sum;
                        }
                        return r;
                    }
                    if (is_matrix(a.type) && is_matrix(b.type)) {
                        int dim = (a.type == GLSL_MAT2) ? 2 :
                                  (a.type == GLSL_MAT3) ? 3 : 4;
                        r.type = a.type;
                        for (int i = 0; i < dim; i++) {
                            for (int j = 0; j < dim; j++) {
                                float sum = 0;
                                for (int k = 0; k < dim; k++) {
                                    sum += a.v[i * dim + k] * b.v[k * dim + j];
                                }
                                r.v[i * dim + j] = sum;
                            }
                        }
                        return r;
                    }
                    /* scalar-vector or vector-vector componentwise */
                    int nc = std::max(ac, bc);
                    r.type = nc > 1 ? (a.type != GLSL_FLOAT && a.type != GLSL_INT && a.type != GLSL_BOOL ? a.type : b.type) : GLSL_FLOAT;
                    if (r.type == GLSL_VOID) r.type = GLSL_FLOAT;
                    for (int i = 0; i < nc && i < MAX_VEC; i++) {
                        float av = (ac == 1) ? a.v[0] : (i < ac ? a.v[i] : 0.0f);
                        float bv = (bc == 1) ? b.v[0] : (i < bc ? b.v[i] : 0.0f);
                        r.v[i] = av * bv;
                    }
                    return r;
                }
                case OP_DIV: {
                    int nc = std::max(ac, bc);
                    r.type = nc > 1 ? a.type : GLSL_FLOAT;
                    if (r.type == GLSL_VOID) r.type = GLSL_FLOAT;
                    for (int i = 0; i < nc && i < MAX_VEC; i++) {
                        float av = (ac == 1) ? a.v[0] : (i < ac ? a.v[i] : 0.0f);
                        float bv = (bc == 1) ? b.v[0] : (i < bc ? b.v[i] : 0.0f);
                        r.v[i] = (bv == 0.0f) ? 0.0f : av / bv;
                    }
                    return r;
                }
                case OP_EQ: {
                    r.set_bool(a.v[0] == b.v[0]);
                    for (int i = 1; i < std::max(ac, bc) && i < MAX_VEC; i++) {
                        if (a.v[i] != b.v[i]) r.set_bool(false);
                    }
                    return r;
                }
                case OP_NE: {
                    r.set_bool(false);
                    for (int i = 0; i < std::max(ac, bc) && i < MAX_VEC; i++) {
                        if (a.v[i] != b.v[i]) r.set_bool(true);
                    }
                    return r;
                }
                case OP_LT: r.set_bool(a.v[0] <  b.v[0]); return r;
                case OP_GT: r.set_bool(a.v[0] >  b.v[0]); return r;
                case OP_LE: r.set_bool(a.v[0] <= b.v[0]); return r;
                case OP_GE: r.set_bool(a.v[0] >= b.v[0]); return r;
                case OP_AND: r.set_bool(a.v[0] != 0.0f && b.v[0] != 0.0f); return r;
                case OP_OR:  r.set_bool(a.v[0] != 0.0f || b.v[0] != 0.0f); return r;
            }
            return r;
        }
        case NK::Unary: {
            Value a = eval(n->operand.get());
            if (n->op == OP_NEG) {
                r = a;
                int nc = type_components(a.type);
                for (int i = 0; i < nc && i < MAX_VEC; i++) r.v[i] = -a.v[i];
                return r;
            }
            if (n->op == OP_NOT) {
                r.set_bool(a.v[0] == 0.0f);
                return r;
            }
            return a;
        }
        case NK::Member: {
            Value o = eval(n->obj.get());
            r.type = (glsl_type_kind_t)(n->swizzle_count == 1 ? GLSL_FLOAT :
                                         GLSL_VEC2 + n->swizzle_count - 2);
            for (int i = 0; i < n->swizzle_count; i++) r.v[i] = o.v[n->swizzle[i]];
            return r;
        }
        case NK::Index: {
            Value o = eval(n->obj.get());
            Value idx = eval(n->idx.get());
            int i = (int)idx.v[0];
            if (i < 0) i = 0;
            if (is_matrix(o.type)) {
                int dim = (o.type == GLSL_MAT2) ? 2 :
                          (o.type == GLSL_MAT3) ? 3 : 4;
                if (i >= dim) i = dim - 1;
                r.type = (glsl_type_kind_t)(GLSL_VEC2 + dim - 2);
                for (int j = 0; j < dim; j++) r.v[j] = o.v[i * dim + j];
            } else {
                int nc = type_components(o.type);
                if (i >= nc) i = nc - 1;
                r.set_float(o.v[i]);
            }
            return r;
        }
        case NK::Assign: {
            LValue lv;
            if (!eval_lvalue(n->target.get(), lv)) return r;
            Value v = eval(n->value.get());
            if (n->op == OP_ASSIGN) {
                assign_lvalue(lv, v);
                return v;
            }
            /* compound assignment: read current, apply op, write back */
            Value cur = *lv.ptr;
            int nc = lv.count;
            Value newv;
            newv.type = v.type;
            int vc = type_components(v.type);
            for (int i = 0; i < nc; i++) {
                float cv = cur.v[lv.swizzle[i]];
                float vv = (vc == 1) ? v.v[0] : (i < vc ? v.v[i] : 0.0f);
                switch (n->op) {
                    case OP_ADD_ASSIGN: newv.v[i] = cv + vv; break;
                    case OP_SUB_ASSIGN: newv.v[i] = cv - vv; break;
                    case OP_MUL_ASSIGN: newv.v[i] = cv * vv; break;
                    case OP_DIV_ASSIGN: newv.v[i] = (vv == 0.0f) ? 0.0f : cv / vv; break;
                    default: newv.v[i] = vv; break;
                }
            }
            assign_lvalue(lv, newv);
            return newv;
        }
        case NK::Ternary: {
            Value c = eval(n->cond.get());
            return (c.v[0] != 0.0f) ? eval(n->then_s.get()) : eval(n->else_s.get());
        }
        case NK::Call: {
            return eval_call(n);
        }
        default:
            return r;
    }
}

bool Interp::eval_lvalue(Node* n, LValue& out) {
    if (!n) return false;
    if (n->kind == NK::Ident) {
        auto it = sh->slot_by_name.find(n->name);
        if (it == sh->slot_by_name.end()) {
            LOGW("glsl", "lvalue: unknown identifier '%s'", n->name.c_str());
            return false;
        }
        out.ptr = &vars[it->second];
        out.swizzle[0] = 0; out.swizzle[1] = 1; out.swizzle[2] = 2; out.swizzle[3] = 3;
        out.count = type_components(out.ptr->type);
        if (out.count < 1) out.count = 1;
        return true;
    }
    if (n->kind == NK::Member) {
        LValue base;
        if (!eval_lvalue(n->obj.get(), base)) return false;
        out.ptr = base.ptr;
        out.count = n->swizzle_count;
        for (int i = 0; i < n->swizzle_count; i++) out.swizzle[i] = n->swizzle[i];
        return true;
    }
    if (n->kind == NK::Index) {
        LValue base;
        if (!eval_lvalue(n->obj.get(), base)) return false;
        Value idx = eval(n->idx.get());
        int i = (int)idx.v[0];
        if (i < 0) i = 0;
        /* For matrices, [i] returns a row (vec). For vectors, [i] is scalar. */
        if (is_matrix(base.ptr->type)) {
            int dim = (base.ptr->type == GLSL_MAT2) ? 2 :
                      (base.ptr->type == GLSL_MAT3) ? 3 : 4;
            if (i >= dim) i = dim - 1;
            out.ptr = base.ptr;
            for (int j = 0; j < dim; j++) out.swizzle[j] = i * dim + j;
            out.count = dim;
        } else {
            int nc = type_components(base.ptr->type);
            if (i >= nc) i = nc - 1;
            out.ptr = base.ptr;
            out.swizzle[0] = i;
            out.count = 1;
        }
        return true;
    }
    LOGW("glsl", "invalid lvalue");
    return false;
}

void Interp::assign_lvalue(const LValue& lv, const Value& v) {
    int vc = type_components(v.type);
    for (int i = 0; i < lv.count; i++) {
        float x = (vc == 1) ? v.v[0] : (i < vc ? v.v[i] : 0.0f);
        lv.ptr->v[lv.swizzle[i]] = x;
    }
}

/* Built-in function dispatch. */
Value Interp::eval_call(Node* n) {
    const std::string& f = n->fname;
    Value r;

    /* Evaluate args. */
    std::vector<Value> av;
    av.reserve(n->args.size());
    for (auto& a : n->args) av.push_back(eval(a.get()));

    auto arg = [&](int i) -> Value& { return av[i]; };

    if (f == "mix") {
        Value x = arg(0), y = arg(1), a = arg(2);
        int nc = std::max(type_components(x.type), type_components(y.type));
        r.type = x.type;
        for (int i = 0; i < nc && i < MAX_VEC; i++) {
            float xv = (type_components(x.type) == 1) ? x.v[0] : x.v[i];
            float yv = (type_components(y.type) == 1) ? y.v[0] : y.v[i];
            float av_ = (type_components(a.type) == 1) ? a.v[0] : a.v[i];
            r.v[i] = xv * (1.0f - av_) + yv * av_;
        }
        return r;
    }
    if (f == "clamp") {
        Value x = arg(0), lo = arg(1), hi = arg(2);
        int nc = type_components(x.type);
        r.type = x.type;
        for (int i = 0; i < nc && i < MAX_VEC; i++) {
            float xv = x.v[i];
            float lov = (type_components(lo.type) == 1) ? lo.v[0] : lo.v[i];
            float hiv = (type_components(hi.type) == 1) ? hi.v[0] : hi.v[i];
            float t = xv < lov ? lov : (xv > hiv ? hiv : xv);
            r.v[i] = t;
        }
        return r;
    }
    if (f == "min" || f == "max") {
        Value x = arg(0), y = arg(1);
        int nc = type_components(x.type);
        r.type = x.type;
        for (int i = 0; i < nc && i < MAX_VEC; i++) {
            float xv = x.v[i];
            float yv = (type_components(y.type) == 1) ? y.v[0] : y.v[i];
            r.v[i] = (f == "min") ? (xv < yv ? xv : yv) : (xv > yv ? xv : yv);
        }
        return r;
    }
    if (f == "abs" || f == "sqrt" || f == "sin" || f == "cos" || f == "pow" ||
        f == "radians" || f == "degrees") {
        Value x = arg(0);
        int nc = type_components(x.type);
        r.type = x.type;
        for (int i = 0; i < nc && i < MAX_VEC; i++) {
            float xv = x.v[i];
            float yv = 0.0f;
            if (f == "pow") { yv = arg(1).v[i]; r.v[i] = powf(xv, yv); }
            else if (f == "abs")     r.v[i] = fabsf(xv);
            else if (f == "sqrt")    r.v[i] = sqrtf(xv);
            else if (f == "sin")     r.v[i] = sinf(xv);
            else if (f == "cos")     r.v[i] = cosf(xv);
            else if (f == "radians") r.v[i] = xv * (3.14159265358979f / 180.0f);
            else if (f == "degrees") r.v[i] = xv * (180.0f / 3.14159265358979f);
        }
        return r;
    }
    if (f == "length") {
        Value x = arg(0);
        int nc = type_components(x.type);
        float s = 0;
        for (int i = 0; i < nc; i++) s += x.v[i] * x.v[i];
        r.set_float(sqrtf(s));
        return r;
    }
    if (f == "normalize") {
        Value x = arg(0);
        int nc = type_components(x.type);
        float s = 0;
        for (int i = 0; i < nc; i++) s += x.v[i] * x.v[i];
        s = sqrtf(s);
        r.type = x.type;
        if (s == 0.0f) {
            for (int i = 0; i < nc; i++) r.v[i] = 0.0f;
        } else {
            for (int i = 0; i < nc; i++) r.v[i] = x.v[i] / s;
        }
        return r;
    }
    if (f == "dot") {
        Value a = arg(0), b = arg(1);
        int nc = std::max(type_components(a.type), type_components(b.type));
        float s = 0;
        for (int i = 0; i < nc; i++) s += a.v[i] * b.v[i];
        r.set_float(s);
        return r;
    }
    if (f == "cross") {
        Value a = arg(0), b = arg(1);
        r.type = GLSL_VEC3;
        r.v[0] = a.v[1] * b.v[2] - a.v[2] * b.v[1];
        r.v[1] = a.v[2] * b.v[0] - a.v[0] * b.v[2];
        r.v[2] = a.v[0] * b.v[1] - a.v[1] * b.v[0];
        return r;
    }
    if (f == "reflect") {
        Value I = arg(0), N = arg(1);
        int nc = type_components(I.type);
        float d = 0;
        for (int i = 0; i < nc; i++) d += N.v[i] * I.v[i];
        r.type = I.type;
        for (int i = 0; i < nc; i++) r.v[i] = I.v[i] - 2.0f * d * N.v[i];
        return r;
    }
    if (f == "texture2D" || f == "texture2DProj" || f == "texture2DLodEXT" || f == "texture2DProjLodEXT") {
        Value s = arg(0);
        Value uv = arg(1);
        if (s.type != GLSL_SAMPLER2D) {
            LOGW("glsl", "texture2D: arg 0 is not a sampler");
            r.type = GLSL_VEC4;
            return r;
        }
        int unit = (int)s.v[0];
        if (unit < 0 || unit >= Program::MAX_TEXTURE_UNITS) {
            LOGW("glsl", "texture2D: sampler unit %d out of range", unit);
            r.type = GLSL_VEC4;
            return r;
        }
        if (!prog->textures_bound[unit]) {
            LOGW("glsl", "texture2D: no texture bound to unit %d", unit);
            r.type = GLSL_VEC4;
            return r;
        }
        float u = uv.v[0];
        float v_ = uv.v[1];
        if (f == "texture2DProj" || f == "texture2DProjLodEXT") {
            if (uv.v[3] != 0.0f) {
                u /= uv.v[3];
                v_ /= uv.v[3];
            }
        }
        return builtin_texture2D(prog, s, u, v_);
    }
    if (f == "textureCube") {
        LOGW("glsl", "textureCube not implemented (no cubemap support)");
        r.type = GLSL_VEC4;
        return r;
    }
    if (f == "vec2" || f == "vec3" || f == "vec4" ||
        f == "ivec2" || f == "ivec3" || f == "ivec4" || f == "mat2" || f == "mat3" || f == "mat4") {
        /* Constructor. */
        if (f == "vec2")      r.type = GLSL_VEC2;
        else if (f == "vec3") r.type = GLSL_VEC3;
        else if (f == "vec4") r.type = GLSL_VEC4;
        else if (f == "ivec2") r.type = GLSL_IVEC2;
        else if (f == "ivec3") r.type = GLSL_IVEC3;
        else if (f == "ivec4") r.type = GLSL_IVEC4;
        else if (f == "mat2") r.type = GLSL_MAT2;
        else if (f == "mat3") r.type = GLSL_MAT3;
        else if (f == "mat4") r.type = GLSL_MAT4;
        int nc = type_components(r.type);
        /* Single scalar to matrix → identity * scalar (diagonal only). */
        if (av.size() == 1 && type_components(av[0].type) == 1 && is_matrix(r.type)) {
            int dim = (r.type == GLSL_MAT2) ? 2 :
                      (r.type == GLSL_MAT3) ? 3 : 4;
            for (int i = 0; i < nc; i++) r.v[i] = 0.0f;
            for (int i = 0; i < dim; i++) r.v[i * dim + i] = av[0].v[0];
        }
        /* Single scalar to vector/scalar → broadcast. */
        else if (av.size() == 1 && type_components(av[0].type) == 1) {
            for (int i = 0; i < nc; i++) r.v[i] = av[0].v[0];
        } else if (av.size() == 1 && type_components(av[0].type) == nc) {
            /* matrix from matrix of same size, or vec from vec */
            for (int i = 0; i < nc; i++) r.v[i] = av[0].v[i];
        } else if (av.size() == 1 && is_matrix(av[0].type)) {
            /* matrix truncation (mat4 → mat3 etc.) */
            int src_dim = (av[0].type == GLSL_MAT2) ? 2 :
                          (av[0].type == GLSL_MAT3) ? 3 : 4;
            int dst_dim = (r.type == GLSL_MAT2) ? 2 :
                          (r.type == GLSL_MAT3) ? 3 : 4;
            for (int i = 0; i < dst_dim; i++)
                for (int j = 0; j < dst_dim; j++)
                    r.v[i * dst_dim + j] = av[0].v[i * src_dim + j];
        } else {
            /* Flatten args. */
            int idx = 0;
            for (auto& a : av) {
                int ac = type_components(a.type);
                for (int i = 0; i < ac && idx < nc; i++) r.v[idx++] = a.v[i];
            }
            /* If we didn't fill all slots and the target is a matrix, the
             * remaining components come from the identity matrix. (GLSL spec.) */
            if (is_matrix(r.type) && idx < nc) {
                int dim = (r.type == GLSL_MAT2) ? 2 :
                          (r.type == GLSL_MAT3) ? 3 : 4;
                for (int col = 0; col < dim; col++) {
                    for (int row = 0; row < dim; row++) {
                        int flat = col * dim + row;
                        if (flat >= idx) {
                            r.v[flat] = (col == row) ? 1.0f : 0.0f;
                        }
                    }
                }
            }
        }
        return r;
    }
    if (f == "float" || f == "int" || f == "bool") {
        if (av.empty()) return r;
        if (f == "float") r.set_float(av[0].v[0]);
        else if (f == "int") r.set_int((int)av[0].v[0]);
        else r.set_bool(av[0].v[0] != 0.0f);
        return r;
    }

    LOGW("glsl", "unknown function '%s'", f.c_str());
    return r;
}

/* Texture sampling with bilinear filter + wrap modes. */
static Value builtin_texture2D(Program* prog, const Value& sampler, float u, float v) {
    int unit = (int)sampler.v[0];
    if (unit < 0 || unit >= Program::MAX_TEXTURE_UNITS || !prog->textures_bound[unit]) {
        Value r; r.type = GLSL_VEC4; return r;
    }
    const glsl_texture_view& t = prog->textures[unit];
    if (t.width <= 0 || t.height <= 0 || !t.rgba) {
        Value r; r.type = GLSL_VEC4; return r;
    }

    /* Wrap. */
    auto wrap_coord = [](float c, int mode, int size) -> float {
        if (mode == GL_REPEAT) {
            c = c - floorf(c);
        } else {
            /* GL_CLAMP_TO_EDGE */
            if (c < 0.0f) c = 0.0f;
            if (c > 1.0f) c = 1.0f;
        }
        return c;
    };
    u = wrap_coord(u, t.wrap_s, t.width);
    v = wrap_coord(v, t.wrap_t, t.height);

    auto fetch = [&](int x, int y) -> std::array<float,4> {
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x >= t.width)  x = t.width  - 1;
        if (y >= t.height) y = t.height - 1;
        const uint8_t* p = t.rgba + (y * t.width + x) * 4;
        return { p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f };
    };

    Value r;
    r.type = GLSL_VEC4;

    if (t.mag_filter == GL_LINEAR) {
        float fx = u * t.width  - 0.5f;
        float fy = v * t.height - 0.5f;
        int x0 = (int)floorf(fx);
        int y0 = (int)floorf(fy);
        int x1 = x0 + 1;
        int y1 = y0 + 1;
        float tx = fx - x0;
        float ty = fy - y0;
        /* Clamp tx/ty to [0,1] in case of edge cases. */
        if (tx < 0) tx = 0; if (tx > 1) tx = 1;
        if (ty < 0) ty = 0; if (ty > 1) ty = 1;
        auto c00 = fetch(x0, y0);
        auto c10 = fetch(x1, y0);
        auto c01 = fetch(x0, y1);
        auto c11 = fetch(x1, y1);
        for (int i = 0; i < 4; i++) {
            float a = c00[i] * (1 - tx) + c10[i] * tx;
            float b = c01[i] * (1 - tx) + c11[i] * tx;
            r.v[i] = a * (1 - ty) + b * ty;
        }
    } else {
        /* GL_NEAREST */
        int x = (int)(u * t.width);
        int y = (int)(v * t.height);
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x >= t.width)  x = t.width  - 1;
        if (y >= t.height) y = t.height - 1;
        auto c = fetch(x, y);
        for (int i = 0; i < 4; i++) r.v[i] = c[i];
    }
    return r;
}

}  // namespace

/* ===================== C API ===================== */

extern "C" {

int glsl_type_components(glsl_type_kind_t t) { return type_components(t); }

glsl_program_t *glsl_compile(const char *vs_src, const char *fs_src) {
    Program* p = new Program();

    /* Lex + parse VS. */
    {
        Parser parser(vs_src ? vs_src : "", &p->vs, p, /*is_vs=*/1);
        parser.parse();
        p->vs.compile_status = parser.sh->compile_status;
        p->vs.info_log = parser.sh->info_log;
        if (!p->vs.compile_status) {
            LOGW("glsl", "VS compile failed: %s", p->vs.info_log.c_str());
        }
    }
    /* Lex + parse FS. */
    {
        Parser parser(fs_src ? fs_src : "", &p->fs, p, /*is_vs=*/0);
        parser.parse();
        p->fs.compile_status = parser.sh->compile_status;
        p->fs.info_log = parser.sh->info_log;
        if (!p->fs.compile_status) {
            LOGW("glsl", "FS compile failed: %s", p->fs.info_log.c_str());
        }
    }
    return (glsl_program_t*)p;
}

void glsl_destroy(glsl_program_t *p) {
    if (!p) return;
    delete (Program*)p;
}

int glsl_link(glsl_program_t *p_, char *info_log, int log_len) {
    Program* p = (Program*)p_;
    if (!p) return -1;
    if (!p->vs.compile_status || !p->fs.compile_status) {
        p->link_status = 0;
        const char* msg = "VS or FS did not compile";
        if (info_log && log_len > 0) {
            strncpy(info_log, msg, log_len - 1);
            info_log[log_len - 1] = 0;
        }
        return -1;
    }
    std::string err;
    if (!link_program(p, err)) {
        p->link_status = 0;
        if (info_log && log_len > 0) {
            strncpy(info_log, err.c_str(), log_len - 1);
            info_log[log_len - 1] = 0;
        }
        LOGW("glsl", "link failed: %s", err.c_str());
        return -1;
    }
    p->link_status = 1;
    if (info_log && log_len > 0) info_log[0] = 0;
    LOGI("glsl", "linked: %d uniforms, %d attributes, %d varyings",
         (int)p->uniforms.size(), (int)p->attributes.size(), (int)p->varyings.size());
    return 0;
}

int glsl_get_vs_compile_status(glsl_program_t *p_) { return ((Program*)p_)->vs.compile_status; }
int glsl_get_fs_compile_status(glsl_program_t *p_) { return ((Program*)p_)->fs.compile_status; }
const char *glsl_get_vs_info_log(glsl_program_t *p_) { return ((Program*)p_)->vs.info_log.c_str(); }
const char *glsl_get_fs_info_log(glsl_program_t *p_) { return ((Program*)p_)->fs.info_log.c_str(); }
int glsl_get_link_status(glsl_program_t *p_) { return ((Program*)p_)->link_status; }

int glsl_get_uniform_location(glsl_program_t *p_, const char *name) {
    Program* p = (Program*)p_;
    if (!p || !name) return -1;
    auto it = p->uniform_by_name.find(name);
    if (it == p->uniform_by_name.end()) return -1;
    return it->second;
}

int glsl_get_attrib_location(glsl_program_t *p_, const char *name) {
    Program* p = (Program*)p_;
    if (!p || !name) return -1;
    auto it = p->attrib_by_name.find(name);
    if (it == p->attrib_by_name.end()) return -1;
    return it->second;
}

int glsl_get_varying_count(glsl_program_t *p_) {
    Program* p = (Program*)p_;
    return p ? (int)p->varyings.size() : 0;
}

int glsl_get_varying_components(glsl_program_t *p_, int slot) {
    Program* p = (Program*)p_;
    if (!p || slot < 0 || slot >= (int)p->varyings.size()) return 0;
    return type_components(p->varyings[slot].type);
}

int glsl_set_uniform(glsl_program_t *p_, int location, int type, int count, const void *value) {
    Program* p = (Program*)p_;
    if (!p || location < 0 || location >= (int)p->uniforms.size() || !value) return -1;
    VarInfo& u = p->uniforms[location];
    Value& dst = p->uniform_values[location];
    /* If the caller passes GLSL_INT but the uniform was declared sampler2D,
     * keep the SAMPLER2D type tag (samplers are set via glUniform1i). */
    if (type == GLSL_INT && u.type == GLSL_SAMPLER2D) {
        dst.type = GLSL_SAMPLER2D;
    } else {
        dst.type = (glsl_type_kind_t)type;
    }
    for (int i = 0; i < MAX_VEC; i++) dst.v[i] = 0.0f;
    int ncomp = type_components((glsl_type_kind_t)type);
    int total = ncomp * count;
    if (total > MAX_VEC) total = MAX_VEC;
    /* GL_INT and GL_BOOL are stored as int32 in the input; convert to float. */
    if (type == GLSL_INT || type == GLSL_BOOL || type == GLSL_IVEC2 || type == GLSL_IVEC3 || type == GLSL_IVEC4 || type == GLSL_SAMPLER2D) {
        const int* ip = (const int*)value;
        for (int i = 0; i < total; i++) dst.v[i] = (float)ip[i];
    } else {
        const float* fp = (const float*)value;
        for (int i = 0; i < total; i++) dst.v[i] = fp[i];
    }
    return 0;
}

int glsl_bind_texture_to_unit(glsl_program_t *p_, int unit, const glsl_texture_view *tex) {
    Program* p = (Program*)p_;
    if (!p || unit < 0 || unit >= Program::MAX_TEXTURE_UNITS) return -1;
    if (tex) {
        p->textures[unit] = *tex;
        p->textures_bound[unit] = 1;
    } else {
        p->textures_bound[unit] = 0;
    }
    return 0;
}

int glsl_run_vertex(glsl_program_t *p_, const glsl_attrib *attribs, int n,
                    float *out_position,
                    float *out_varyings, int varying_count) {
    Program* p = (Program*)p_;
    if (!p || !p->link_status) return -1;
    Shader* s = &p->vs;

    /* Reset storage. */
    Value* vars = s->var_storage.data();
    for (size_t i = 0; i < s->var_storage.size(); i++) {
        vars[i].type = s->vars[i].type;
        for (int j = 0; j < MAX_VEC; j++) vars[i].v[j] = 0.0f;
    }

    /* Copy uniform values into shader slots. */
    for (auto& v : s->vars) {
        if (v.qualifier == 2 /* uniform */ && v.uniform_location >= 0) {
            vars[v.slot] = p->uniform_values[v.uniform_location];
        }
    }

    /* Copy attributes into shader slots. */
    for (int i = 0; i < n; i++) {
        const glsl_attrib& a = attribs[i];
        /* Find the attribute with this index in the program. */
        if (a.index < 0 || a.index >= (int)p->attributes.size()) continue;
        VarInfo& av = p->attributes[a.index];
        /* Find the slot in the shader. */
        auto it = s->slot_by_name.find(av.name);
        if (it == s->slot_by_name.end()) continue;
        int slot = it->second;
        Value& dst = vars[slot];
        dst.type = av.type;
        for (int j = 0; j < MAX_VEC; j++) dst.v[j] = 0.0f;
        /* Read `size` components of `type`. */
        const uint8_t* bytes = (const uint8_t*)a.value;
        if (!bytes) continue;
        for (int c = 0; c < a.size && c < 4; c++) {
            float fv = 0.0f;
            switch (a.type) {
                case GL_FLOAT:          fv = ((const float*)bytes)[c]; break;
                case GL_UNSIGNED_BYTE:  {
                    uint8_t b = bytes[c];
                    fv = a.normalized ? (b / 255.0f) : (float)b;
                    break;
                }
                case GL_BYTE: {
                    int8_t b = (int8_t)bytes[c];
                    fv = a.normalized ? (b < 0 ? (b + 256.0f) / 255.0f : b / 255.0f) : (float)b;
                    break;
                }
                case GL_SHORT: {
                    int16_t v16 = ((const int16_t*)bytes)[c];
                    fv = a.normalized ? (v16 < 0 ? (v16 + 32768.0f) / 32767.0f : v16 / 32767.0f) : (float)v16;
                    break;
                }
                case GL_UNSIGNED_SHORT: {
                    uint16_t v16 = ((const uint16_t*)bytes)[c];
                    fv = a.normalized ? (v16 / 65535.0f) : (float)v16;
                    break;
                }
                case GL_INT: {
                    int32_t v32 = ((const int32_t*)bytes)[c];
                    fv = a.normalized ? (v32 < 0 ? (v32 + 2147483648.0f) / 2147483647.0f : (float)v32 / 2147483647.0f) : (float)v32;
                    break;
                }
                case GL_UNSIGNED_INT: {
                    uint32_t v32 = ((const uint32_t*)bytes)[c];
                    fv = a.normalized ? (v32 / 4294967295.0f) : (float)v32;
                    break;
                }
                default: fv = 0.0f; break;
            }
            dst.v[c] = fv;
        }
    }

    /* Run the AST. */
    Interp it;
    it.prog = p;
    it.sh = s;
    it.vars = vars;
    it.return_flag = 0;
    it.discard_flag = 0;
    if (s->main_body) it.exec_block(s->main_body.get());

    /* Read gl_Position. */
    if (s->slot_gl_position >= 0) {
        Value& pos = vars[s->slot_gl_position];
        if (out_position) {
            out_position[0] = pos.v[0];
            out_position[1] = pos.v[1];
            out_position[2] = pos.v[2];
            out_position[3] = pos.v[3];
        }
    } else if (out_position) {
        out_position[0] = out_position[1] = out_position[2] = 0.0f;
        out_position[3] = 1.0f;
    }

    /* Write varyings. */
    if (out_varyings) {
        for (int i = 0; i < varying_count * 4; i++) out_varyings[i] = 0.0f;
        for (size_t i = 0; i < s->vars.size(); i++) {
            VarInfo& v = s->vars[i];
            if (v.qualifier == 3 /* varying */ && v.varying_slot >= 0 && v.varying_slot < varying_count) {
                int nc = type_components(v.type);
                Value& val = vars[i];
                for (int c = 0; c < nc; c++) {
                    out_varyings[v.varying_slot * 4 + c] = val.v[c];
                }
            }
        }
    }
    return 0;
}

int glsl_run_fragment(glsl_program_t *p_, const float *varyings, int varying_count,
                      const float *frag_coord,
                      float *out_color) {
    Program* p = (Program*)p_;
    if (!p || !p->link_status) return -1;
    Shader* s = &p->fs;

    Value* vars = s->var_storage.data();
    for (size_t i = 0; i < s->var_storage.size(); i++) {
        vars[i].type = s->vars[i].type;
        for (int j = 0; j < MAX_VEC; j++) vars[i].v[j] = 0.0f;
    }

    /* Copy uniform values. */
    for (auto& v : s->vars) {
        if (v.qualifier == 2 && v.uniform_location >= 0) {
            vars[v.slot] = p->uniform_values[v.uniform_location];
        }
    }
    /* Copy varyings. */
    if (varyings) {
        for (size_t i = 0; i < s->vars.size(); i++) {
            VarInfo& v = s->vars[i];
            if (v.qualifier == 3 && v.varying_slot >= 0 && v.varying_slot < varying_count) {
                int nc = type_components(v.type);
                for (int c = 0; c < nc; c++) {
                    vars[i].v[c] = varyings[v.varying_slot * 4 + c];
                }
            }
        }
    }
    /* gl_FragCoord. */
    if (s->slot_gl_frag_coord >= 0 && frag_coord) {
        Value& fc = vars[s->slot_gl_frag_coord];
        fc.type = GLSL_VEC4;
        fc.v[0] = frag_coord[0];
        fc.v[1] = frag_coord[1];
        fc.v[2] = frag_coord[2];
        fc.v[3] = frag_coord[3];
    }

    Interp it;
    it.prog = p;
    it.sh = s;
    it.vars = vars;
    it.return_flag = 0;
    it.discard_flag = 0;
    if (s->main_body) it.exec_block(s->main_body.get());

    if (out_color) {
        if (s->slot_gl_frag_color >= 0) {
            Value& c = vars[s->slot_gl_frag_color];
            out_color[0] = c.v[0];
            out_color[1] = c.v[1];
            out_color[2] = c.v[2];
            out_color[3] = c.v[3];
        } else {
            out_color[0] = out_color[1] = out_color[2] = 0.0f;
            out_color[3] = 1.0f;
        }
    }
    return it.discard_flag ? 1 : 0;
}

}  // extern "C"
