///
/// @file
/// @brief eForth implemented in 100% C/C++ for portability and education
///
#include <string.h>      // strcmp
#include <strings.h>     // strcasecmp
#include "ceforth.h"
///====================================================================
///
///> Global memory blocks
///
/// Note:
///   1.By separating pmem from dictionary,
///   * it makes dictionary uniform size which eliminates the need for link field
///   * however, it requires array size tuning manually
///   2.Using 16-bit xt offset in parameter field (instead of full 32 or 64 bits),
///   * it unified xt/pfa parameter storage and use the LSB for id flag
///   * that compacts memory usage while avoiding the double lookup of
///   * token threaded indexing.
///   * However, it limits function pointer spread within range of 64KB
///   3.For ease of byte counting, U8* is used for pmem instead of U16*.
///   * this makes IP increment by 2 instead of word size.
///   * If needed, it can be readjusted.
///
///> Dictionary structure (N=E4_DICT_SZ in config.h)
///     dict[0].xt ---------> pointer to build-in word lambda[0]
///     dict[1].xt ---------> pointer to built-in word lambda[1]
///     ...
///     dict[N-1].xt -------> pointer to last built-in word lambda[N-1]
///
///> Parameter memory structure (memory block=E4_PMEM_SZ in config.h)
///     dict[N].xt ----+ user defined colon word)    dict[N+1].xt------+
///                    |                                               |
///     +--MEM0        v                                               v
///     +--------------+--------+--------+-----+------+----------------+-----
///     | str nameN \0 |  parm1 |  parm2 | ... | ffff | str nameN+1 \0 | ...
///     +--------------+--------+--------+-----+------+----------------+-----
///     ^              ^        ^        ^     ^      ^
///     | strlen+1     | 2-byte | 2-byte |     |      |
///     +--------------+--------+--------+-----+------+---- 2-byte aligned
///
///> Parameter structure - 16-bit aligned (use MSB for colon/primitive word flag)
///   * primitive word
///     16-bit xt offset with MSB set to 1, where opcode < MAX_OP
///     +-+--------------+
///     |1|   opcode     |   call exec_prim(opcode)
///     +-+--------------+
///
///   * colon word (user defined)
///     16-bit pmem offset with MSB set to 1, where dict.pfa >= MAX_OP
///     +--------------+-+
///     |1|   dict.pfa   |   IP = dict.pfa
///     +--------------+-+
///
///   * built-in word
///     16-bit xt offset with MSB set to 0 (1 less mem lookup for xt)
///     +-+--------------+
///     |0| dict.xtoff() |   call (XT0 + *IP)() to execute
///     +-+--------------+
///
List<DU,   E4_RS_SZ>   rs;         ///< return stack
List<DU,   E4_SS_SZ>   ss;         ///< parameter stack
List<Code, E4_DICT_SZ> dict;       ///< dictionary
List<U8,   E4_PMEM_SZ> pmem;       ///< parameter memory (for colon definitions)
U8  *MEM0 = &pmem[0];              ///< base of parameter memory block
///
///> Macros to abstract dict and pmem physical implementation
///  Note:
///    so we can change pmem implementation anytime without affecting opcodes defined below
///
///@name Dictionary and data stack access macros
///@{
#define BOOL(f)   ((f)?-1:0)               /**< Forth boolean representation            */
#define HERE      (pmem.idx)               /**< current parameter memory index          */
#define LAST      (dict[dict.idx-1])       /**< last colon word defined                 */
#define MEM(a)    (MEM0 + (IU)UINT(a))     /**< pointer to address fetched from pmem    */
#define IGET(ip)  (*(IU*)MEM(ip))          /**< instruction fetch from pmem+ip offset   */
#define CELL(a)   (*(DU*)&pmem[a])         /**< fetch a cell from parameter memory      */
#define SETJMP(a) (*(IU*)&pmem[a] = HERE)  /**< address offset for branching opcodes    */
///@}
///
/// primitive words to simplify compiler
///
typedef enum {
    EXIT=0|EXT_FLAG, NOP, NEXT, LOOP, LIT, VAR, STR, DOTQ, BRAN, ZBRAN,
    VBRAN, DOES, FOR, DO, KEY, MAX_OP
} forth_opcode;

Code op_prim[] = {
    Code(";",    EXIT), Code("nop",  NOP),   Code("next",  NEXT),  Code("loop",  LOOP),
    Code("lit",  LIT),  Code("var",  VAR),   Code("str",   STR),   Code("dotq",  DOTQ),
    Code("bran", BRAN), Code("0bran",ZBRAN), Code("vbran", VBRAN), Code("does>", DOES),
    Code("for",  FOR),  Code("do",   DO),    Code("key",   KEY)
};
#define USER_AREA  (ALIGN16(MAX_OP & ~EXT_FLAG))
#define IS_PRIM(w) ((w & EXT_FLAG) && (w < MAX_OP))

///====================================================================
///
///> Platform specific code - deferred
///
void key();                               // read key from console
void dict_dump();                         // dump dictionary
void mem_stat();                          // display memory statistics
void native_api();                        // call native/Javascript
int  forth_include(const char *fn);       // load external Forth script

///====================================================================
///
///> VM states (single task)
///
typedef enum { STOP=0, HOLD, QUERY, NEST, IO } vm_state;

IU       IP = 0;        ///< instruction pointer
vm_state VM = QUERY;    ///< VM state
///
///> user variables
///
DU   top     = -DU1;    ///< top of stack (cached)
bool compile = false;   ///< compiler flag
bool ucase   = false;   ///< case sensitivity control
IU   *base;             ///< numeric radix (a pointer)
IU   *dflt;             ///< use float data unit flag
///
///====================================================================
///
///> Dictionary search functions - can be adapted for ROM+RAM
///
IU find(const char *s) {
    auto streq = [](const char *s1, const char *s2) {
        return ucase ? strcasecmp(s1, s2)==0 : strcmp(s1, s2)==0;
    };
    IU v = 0;
    for (IU i = dict.idx - 1; !v && i > 0; --i) {
        if (streq(s, dict[i].name)) v = i;
    }
#if CC_DEBUG > 1
    LOG_HDR("find", s); if (v) { LOG_DIC(v); } else LOG_NA();
#endif // CC_DEBUG > 1
    return v;
}
///====================================================================
///
///> Colon word compiler
///  Note:
///    * we separate dict and pmem space to make word uniform in size
///    * if they are combined then can behaves similar to classic Forth
///    * with an addition link field added.
///
void colon(const char *name) {
    char *nfa = (char*)&pmem[HERE]; ///> current pmem pointer
    int sz = STRLEN(name);          ///> string length, aligned
    pmem.push((U8*)name,  sz);      ///> setup raw name field

    Code c(nfa, (FPTR)~0, false);   ///> create a local blank word
    c.attr = UDF_ATTR;              ///> specify a colon (user defined) word
    c.pfa  = HERE;                  ///> capture code field index

    dict.push(c);                   ///> deep copy Code struct into dictionary
}
void add_iu(IU i) { pmem.push((U8*)&i, sizeof(IU)); }  ///< add an instruction into pmem
void add_du(DU v) { pmem.push((U8*)&v, sizeof(DU)); }  ///< add a cell into pmem
int  add_str(const char *s) {       ///< add a string to pmem
    int sz = STRLEN(s);
    pmem.push((U8*)s,  sz);         /// * add string terminated with zero
    return sz;
}
void add_w(IU w) {                  ///< add a word index into pmem
    Code &c = IS_PRIM(w)            /// * is primitive?
        ? op_prim[w & ~EXT_FLAG]    /// * ref to primitive word entry
        : dict[w];                  /// * ref to dictionary entry
    IU ip = (w & EXT_FLAG)          /// * is primitive?
        ? (UFP)c.xt                 /// * get primitive/built-in token
        : (c.attr & UDF_ATTR        /// * colon word?
           ? (c.pfa | EXT_FLAG)     /// * pfa with colon word flag
           : c.xtoff());            /// * XT offset of built-in
    add_iu(ip);
#if CC_DEBUG > 1
    LOG_KV("add_w(", w); LOG_KX(") => ", ip);
    LOGS(" "); LOGS(c.name); LOGS("\n");
#endif // CC_DEBUG > 1
}
void add_var(IU op) {               ///< add a varirable header
    add_w(op);                      /// * VAR or VBRAN
    if (op==VBRAN) add_iu(0);       /// * pad offset field
#if DO_WASM
    pmem.idx = DALIGN(pmem.idx);    /// * data alignment (WASM 4, other 2)
#endif // DO_WASM
    if (op==VAR)   add_du(DU0);     /// * default variable = 0
}
///====================================================================
///
/// utilize C++ standard template libraries for core IO functions only
/// Note:
///   * we use STL for its convinence, but
///   * if it takes too much memory for target MCU,
///   * these functions can be replaced with our own implementation
///
#include <iomanip>                  /// setbase, setw, setfill
#include <sstream>                  /// iostream, stringstream
using namespace std;                /// default to C++ standard template library
istringstream   fin;                ///< forth_in
ostringstream   fout;               ///< forth_out
string          pad;                ///< input string buffer
void (*fout_cb)(int, const char*);  ///< forth output callback function (see ENDL macro)
///====================================================================
///
/// macros to reduce verbosity
///
int def_word(const char* name) {                ///< display if redefined
    if (name[0]=='\0') { fout << " name?" << ENDL; return 0; }  /// * missing name?
    if (find(name)) {                           /// * word redefined?
        fout << name << " reDef? " << ENDL;
    }
    colon(name);                                /// * create a colon word
    return 1;                                   /// * created OK
}
char *word() {                                  ///< get next idiom
    if (!(fin >> pad)) pad.clear();             /// * input buffer exhausted?
    return (char*)pad.c_str();
}
inline char *scan(char c) { getline(fin, pad, c); return (char*)pad.c_str(); }
inline void PUSH(DU v) { ss.push(top); top = v; }
inline DU   POP()      { DU n=top; top=ss.pop(); return n; }
///====================================================================
///
///> Forth inner interpreter (handles a colon word)
///  Note:
///  * overhead here in C call/return vs NEXT threading (in assembly)
///  * use of dp (iterative depth control) instead of WP by Dr. Ting
///    speeds up 8% vs recursive calls but loose the flexibity of Forth
///  * use of cached _NXT address speeds up 10% on AMD but
///    5% slower on ESP32 probably due to shallow pipeline
///  * computed label runs 15% faster, but needs long macros (for enum)
///  * use local stack speeds up 10%, but allot 4*64 bytes extra
///
///  TODO: performance tuning
///    1. Just-in-time cache(ip, dp)
///    2. Co-routine
///
#define DISPATCH(op) switch(op)
#define CASE(op, g)  case op : { g; } break
#define OTHER(g)     default : { g; } break
#define UNNEST()     (VM = (IP=UINT(rs.pop())) ? HOLD : STOP)

void rs_dump(const char *hdr) {
    printf("%s [", hdr);
    for (int i=0; i<rs.idx; i++) printf(" %04x", static_cast<int>(rs[i]));
    printf(" ]\n");
}
void nest() {
    rs_dump("nest>>");
    VM = NEST;                                       /// * activate VM
    while (VM==NEST && IP) {
        IU ix = IGET(IP);                            ///> fetched opcode, hopefully in register
        printf("[%4x]:%4x", IP, ix);
        IP += sizeof(IU);
        DISPATCH(ix) {                               /// * opcode dispatcher
        CASE(EXIT, UNNEST());
        CASE(NOP,  { /* do nothing */});
        CASE(NEXT,
             if (GT(rs[-1] -= DU1, -DU1)) {          ///> loop done?
                 IP = IGET(IP);                      /// * no, loop back
             }
             else {                                  /// * yes, loop done!
                 rs.pop();                           /// * pop off loop counter
                 IP += sizeof(IU);                   /// * next instr.
             });
        CASE(LOOP,
             if (GT(rs[-2], rs[-1] += DU1)) {        ///> loop done?
                 IP = IGET(IP);                      /// * no, loop back
             }
             else {                                  /// * yes, done
                 rs.pop(); rs.pop();                 /// * pop off counters
                 IP += sizeof(IU);                   /// * next instr.
             });
        CASE(LIT,
             ss.push(top);
             top = *(DU*)MEM(IP);                    ///> from hot cache, hopefully
             IP += sizeof(DU));                      /// * hop over the stored value
        CASE(VAR, PUSH(DALIGN(IP)); UNNEST());       ///> get var addr, alignment?
        CASE(STR,
             const char *s = (const char*)MEM(IP);   ///< get string pointer
             IU    len = STRLEN(s);
             PUSH(IP); PUSH(len); IP += len);
        CASE(DOTQ,                                   /// ." ..."
             const char *s = (const char*)MEM(IP);   ///< get string pointer
             fout << s;  IP += STRLEN(s));           /// * send to output console
        CASE(BRAN, IP = IGET(IP));                   /// * unconditional branch
        CASE(ZBRAN,                                  /// * conditional branch
             IP = POP() ? IP+sizeof(IU) : IGET(IP));
        CASE(VBRAN,
             PUSH(DALIGN(IP + sizeof(IU)));          /// * skip target address
             if ((IP = IGET(IP))==0) UNNEST());      /// * jump target of does> if given
        CASE(DOES,
             IU *p = (IU*)MEM(LAST.pfa);             ///< memory pointer to pfa 
             *(p+1) = IP;                            /// * encode current IP, and bail
             UNNEST());
        CASE(FOR,  rs.push(POP()));                  /// * setup FOR..NEXT call frame
        CASE(DO,                                     /// * setup DO..LOOP call frame
             rs.push(ss.pop()); rs.push(POP()));
        CASE(KEY,  key(); VM = IO);                  /// * fetch single keypress
        OTHER(
            if (ix & EXT_FLAG) {                     /// * colon word?
                rs.push(IP);                         /// * setup call frame
                IP = ix & ~EXT_FLAG;                 /// * IP = word.pfa
            }
            else Code::exec(ix));                    ///> execute built-in word
        }
        printf("   => IP=%4x, rs.idx=%d, VM=%d\n", IP, rs.idx, VM);
        rs_dump("nest <<");
    }
}
///
///> CALL - inner-interpreter proxy (inline macro does not run faster)
///
void CALL(IU w) {
    rs_dump("call");
    if (IS_UDF(w)) {                   /// colon word
        rs.push(DU0);
        IP = dict[w].pfa;              /// setup task context
        nest();
    }
    else dict[w].call();               /// built-in word
}
///====================================================================
///
///> IO & debug functions
///
void spaces(int n)  { for (int i = 0; i < n; i++) fout << " "; }
void s_quote(forth_opcode op) {
    const char *s = scan('"')+1;       ///> string skip first blank
    if (compile) {
        add_w(op);                     ///> dostr, (+parameter field)
        add_str(s);                    ///> byte0, byte1, byte2, ..., byteN
    }
    else {                             ///> use PAD ad TEMP storage
        IU h0  = HERE;                 ///> keep current memory addr
        DU len = add_str(s);           ///> write string to PAD
        PUSH(h0);                      ///> push string address
        PUSH(len);                     ///> push string length
        HERE = h0;                     ///> restore memory addr
    }
}
///
/// recursively disassemble colon word
///
#define TONAME(w) (dict[w].pfa - STRLEN(dict[w].name))
int pfa2didx(IU ix) {                          ///> reverse lookup
    if (IS_PRIM(ix)) return (int)ix;           ///> primitives
    IU pfa = ix & ~EXT_FLAG;                   ///> pfa (mask colon word)
    for (int i = dict.idx - 1; i > 0; --i) {
        if (ix & EXT_FLAG) {                   /// colon word?
            if (dict[i].pfa == pfa) return i;  ///> compare pfa in PMEM
        }
        else if (dict[i].xtoff() == pfa) return i;   ///> compare xt (built-in words)
    }
    return 0;                                  /// * not found
}
int  pfa2nvar(IU pfa) {
    IU  w  = IGET(pfa);
    if (w != VAR && w != VBRAN) return 0;
    
    IU  i0 = pfa2didx(pfa | EXT_FLAG);
    if (!i0) return 0;
    IU  p1 = (i0+1) < dict.idx ? TONAME(i0+1) : HERE;
    int n  = p1 - pfa - sizeof(IU) * (w==VAR ? 1 : 2);    ///> CC: calc # of elements
    return n;
}
void to_s(IU w, U8 *ip) {
#if CC_DEBUG
    fout << setbase(16) << "( ";
    fout << setfill('0') << setw(4) << (ip - MEM0);       ///> addr
    fout << '[' << setfill(' ') << setw(4) << w << ']';   ///> word ref
    fout << " ) " << setbase(*base);
#endif // CC_DEBUG
    
    ip += sizeof(IU);                  ///> calculate next ip
    switch (w) {
    case LIT:  fout << *(DU*)ip << " ( lit )";      break;
    case STR:  fout << "s\" " << (char*)ip << '"';  break;
    case DOTQ: fout << ".\" " << (char*)ip << '"';  break;
    case VAR:
    case VBRAN: {
        int n  = pfa2nvar(UINT(ip - MEM0 - sizeof(IU)));
        IU  ix = (IU)(ip - MEM0 + (w==VAR ? 0 : sizeof(IU)));
        for (int i = 0, a=DALIGN(ix); i < n; i+=sizeof(DU)) {
            fout << *(DU*)MEM(a + i) << ' ';
        }
    }                                               /// no break, fall through
    default:
        Code &c = IS_PRIM(w) ? op_prim[w & ~EXT_FLAG] : dict[w];
        fout << c.name; break;
    }
    switch (w) {
    case NEXT: case LOOP:
    case BRAN: case ZBRAN: case VBRAN:             ///> display jmp target
        fout << ' ' << setfill('0') << setw(4) << *(IU*)ip;
        break;
    default: /* do nothing */ break;
    }
    fout << setfill(' ') << setw(-1);   ///> restore output format settings
}
void see(IU pfa) {
    U8 *ip = MEM(pfa);
    while (1) {
        IU w = pfa2didx(*(IU*)ip);      ///> fetch word index by pfa
        if (!w) break;                  ///> loop guard
        
        fout << ENDL; fout << "  ";     /// * indent
        to_s(w, ip);                    /// * display opcode
        if (w==EXIT || w==VAR) return;  /// * end of word
        
        ip += sizeof(IU);               ///> advance ip (next opcode)
        switch (w) {                    ///> extra bytes to skip
        case LIT:   ip += sizeof(DU);                    break; /// alignment?
        case STR:   case DOTQ:  ip += STRLEN((char*)ip); break;
        case BRAN:  case ZBRAN:
        case NEXT:  case LOOP:  ip += sizeof(IU);        break;
        case VBRAN: ip = MEM(*(IU*)ip);                  break;
        }
    }
}
void words() {
    const int WIDTH = 60;
    int sz = 0;
    fout << setbase(10);
    for (int i=0; i<dict.idx; i++) {
        const char *nm = dict[i].name;
        const int  len = strlen(nm);
#if CC_DEBUG > 1
        if (nm[0]) {
#else  //  CC_DEBUG > 1
        if (nm[len-1] != ' ') {
#endif // CC_DEBUG > 1
            sz += len + 2;
            fout << "  " << nm;
        }
        if (sz > WIDTH) {
            sz = 0;
            fout << ENDL;
            yield();
        }
    }
    fout << setbase(*base) << ENDL;
}
void ss_dump(bool forced=false) {
#if DO_WASM    
    if (!forced) { fout << "ok" << ENDL; return; }
#endif //    
    static char buf[34];
    auto rdx = [](DU v, int b) {          ///> display v by radix
#if USE_FLOAT
        sprintf(buf, "%0.6g", v);
        return buf;
#else // !USE_FLOAT
        int i = 33;  buf[i]='\0';         /// * C++ can do only base=8,10,16
        DU  n = ABS(v);                   ///< handle negative
        do {                              ///> digit-by-digit
            U8 d = (U8)MOD(n,b);  n /= b;
            buf[--i] = d > 9 ? (d-10)+'a' : d+'0';
        } while (n && i);
        if (v < DU0) buf[--i]='-';
        return &buf[i];
#endif // USE_FLOAT
    };
    ss.push(top);
    for (int i=0; i<ss.idx; i++) {
        fout << rdx(ss[i], *base) << ' ';
    }
    top = ss.pop();
    fout << "-> ok" << ENDL;
}
void mem_dump(U32 p0, IU sz) {
    fout << setbase(16) << setfill('0');
    for (IU i=ALIGN16(p0); i<=ALIGN16(p0+sz); i+=16) {
        fout << setw(4) << i << ": ";
        for (int j=0; j<16; j++) {
            U8 c = pmem[i+j];
            fout << setw(2) << (int)c << (MOD(j,4)==3 ? " " : "");
        }
        for (int j=0; j<16; j++) {   // print and advance to next byte
            U8 c = pmem[i+j] & 0x7f;
            fout << (char)((c==0x7f||c<0x20) ? '_' : c);
        }
        fout << ENDL;
        yield();
    }
    fout << setbase(*base) << setfill(' ');
}
void load(const char* fn) {
    rs.push(IP);                          /// * save context
    VM = NEST;                            /// * +recursive
    forth_include(fn);                    /// * include file
    IP = UINT(rs.pop());                  /// * restore context
}
///====================================================================
///
///> eForth dictionary assembler
///  Note: sequenced by enum forth_opcode as following
///
UFP Code::XT0 = ~0;    ///< init base of xt pointers (before calling CODE macros)

void dict_compile() {  ///< compile built-in words into dictionary
    CODE("nul ",    {});                  /// dict[0], not used, simplify find()
    ///
    /// @defgroup Stack ops
    /// @brief - opcode sequence can be changed below this line
    /// @{
    CODE("dup",     PUSH(top));
    CODE("drop",    top = ss.pop());
    CODE("over",    PUSH(ss[-1]));
    CODE("swap",    DU n = ss.pop(); PUSH(n));
    CODE("rot",     DU n = ss.pop(); DU m = ss.pop(); ss.push(n); PUSH(m));
    CODE("-rot",    DU n = ss.pop(); DU m = ss.pop(); PUSH(m); PUSH(n));
    CODE("nip",     ss.pop());
    CODE("pick",    DU i = top; top = ss[-i]);
    /// @}
    /// @defgroup Stack ops - double
    /// @{
    CODE("2dup",    PUSH(ss[-1]); PUSH(ss[-1]));
    CODE("2drop",   ss.pop(); top = ss.pop());
    CODE("2over",   PUSH(ss[-3]); PUSH(ss[-3]));
    CODE("2swap",   DU n = ss.pop(); DU m = ss.pop(); DU l = ss.pop();
                    ss.push(n); PUSH(l); PUSH(m));
    CODE("?dup",    if (top != DU0) PUSH(top));
    /// @}
    /// @defgroup ALU ops
    /// @{
    CODE("+",       top += ss.pop());
    CODE("*",       top *= ss.pop());
    CODE("-",       top =  ss.pop() - top);
    CODE("/",       top =  ss.pop() / top);
    CODE("mod",     top =  MOD(ss.pop(), top));
    CODE("*/",      top =  (DU2)ss.pop() * ss.pop() / top);
    CODE("/mod",    DU  n = ss.pop();
                    DU  t = top;
                    DU  m = MOD(n, t);
                    ss.push(m); top = UINT(n / t));
    CODE("*/mod",   DU2 n = (DU2)ss.pop() * ss.pop();
                    DU2 t = top;
                    DU  m = MOD(n, t);
                    ss.push(m); top = UINT(n / t));
    CODE("and",     top = UINT(top) & UINT(ss.pop()));
    CODE("or",      top = UINT(top) | UINT(ss.pop()));
    CODE("xor",     top = UINT(top) ^ UINT(ss.pop()));
    CODE("abs",     top = ABS(top));
    CODE("negate",  top = -top);
    CODE("invert",  top = ~UINT(top));
    CODE("rshift",  top = UINT(ss.pop()) >> UINT(top));
    CODE("lshift",  top = UINT(ss.pop()) << UINT(top));
    CODE("max",     DU n=ss.pop(); top = (top>n) ? top : n);
    CODE("min",     DU n=ss.pop(); top = (top<n) ? top : n);
    CODE("2*",      top *= 2);
    CODE("2/",      top /= 2);
    CODE("1+",      top += 1);
    CODE("1-",      top -= 1);
#if USE_FLOAT
    CODE("int",     top = UINT(top));         // float => integer
#endif // USE_FLOAT
    /// @}
    /// @defgroup Logic ops
    /// @{
    CODE("0=",      top = BOOL(ZEQ(top)));
    CODE("0<",      top = BOOL(LT(top, DU0)));
    CODE("0>",      top = BOOL(GT(top, DU0)));
    CODE("=",       top = BOOL(EQ(ss.pop(), top)));
    CODE(">",       top = BOOL(GT(ss.pop(), top)));
    CODE("<",       top = BOOL(LT(ss.pop(), top)));
    CODE("<>",      top = BOOL(!EQ(ss.pop(), top)));
    CODE(">=",      top = BOOL(!LT(ss.pop(), top)));
    CODE("<=",      top = BOOL(!GT(ss.pop(), top)));
    CODE("u<",      top = BOOL(UINT(ss.pop()) < UINT(top)));
    CODE("u>",      top = BOOL(UINT(ss.pop()) > UINT(top)));
    /// @}
    /// @defgroup IO ops
    /// @{
    CODE("case!",   ucase = POP() == DU0);    // case insensitive
    CODE("base",    PUSH(((U8*)base - MEM0)));
    CODE("decimal", fout << setbase(*base = 10));
    CODE("hex",     fout << setbase(*base = 16));
    CODE("bl",      fout << " ");
    CODE("cr",      fout << ENDL);
    CODE(".",       fout << POP() << " ");
    CODE("u.",      fout << UINT(POP()) << " ");
    CODE(".r",      fout << setbase(*base) << setw(POP()) << POP());
    CODE("u.r",     fout << setbase(*base) << setw(POP()) << UINT(POP()));
    CODE("type",    POP();                    // string length (not used)
         fout << (const char*)MEM(POP()));    // get string pointer
    IMMD("key",     if (compile) add_w(KEY); else key());
    CODE("emit",    char b = (char)POP(); fout << b);
    CODE("space",   spaces(1));
    CODE("spaces",  spaces(POP()));
    /// @}
    /// @defgroup Literal ops
    /// @{
    CODE("[",       compile = false);
    CODE("]",       compile = true);
    IMMD("(",       scan(')'));
    IMMD(".(",      fout << scan(')'));
    IMMD("\\",      scan('\n'));
    IMMD("s\"",     s_quote(STR));
    IMMD(".\"",     s_quote(DOTQ));
    /// @}
    /// @defgroup Branching ops
    /// @brief - if...then, if...else...then
    /// @{
    IMMD("if",      add_w(ZBRAN); PUSH(HERE); add_iu(0));       // if    ( -- here )
    IMMD("else",                                                // else ( here -- there )
         add_w(BRAN);
         IU h=HERE; add_iu(0); SETJMP(POP()); PUSH(h));
    IMMD("then",    SETJMP(POP()));                             // backfill jump address
    /// @}
    /// @defgroup Loops
    /// @brief  - begin...again, begin...f until, begin...f while...repeat
    /// @{
    IMMD("begin",   PUSH(HERE));
    IMMD("again",   add_w(BRAN);  add_iu(POP()));               // again    ( there -- )
    IMMD("until",   add_w(ZBRAN); add_iu(POP()));               // until    ( there -- )
    IMMD("while",   add_w(ZBRAN); PUSH(HERE); add_iu(0));       // while    ( there -- there here )
    IMMD("repeat",  add_w(BRAN);                                // repeat    ( there1 there2 -- )
         IU t=POP(); add_iu(POP()); SETJMP(t));                 // set forward and loop back address
    /// @}
    /// @defgrouop FOR...NEXT loops
    /// @brief  - for...next, for...aft...then...next
    /// @{
    IMMD("for" ,    add_w(FOR); PUSH(HERE));                    // for ( -- here )
    IMMD("next",    add_w(NEXT); add_iu(POP()));                // next ( here -- )
    IMMD("aft",                                                 // aft ( here -- here there )
         POP(); add_w(BRAN);
         IU h=HERE; add_iu(0); PUSH(HERE); PUSH(h));
    /// @}
    /// @}
    /// @defgrouop DO..LOOP loops
    /// @{
    IMMD("do" ,     add_w(DO); PUSH(HERE));                     // for ( -- here )
    CODE("i",       PUSH(rs[-1]));
    CODE("leave",   rs.pop(); rs.pop(); UNNEST());              // quit DO..LOOP
    IMMD("loop",    add_w(LOOP); add_iu(POP()));                // next ( here -- )
    /// @}
    /// @defgrouop return stack ops
    /// @{
    CODE(">r",      rs.push(POP()));
    CODE("r>",      PUSH(rs.pop()));
    CODE("r@",      PUSH(rs[-1]));                              // same as I (the loop counter)
    /// @}
    /// @defgrouop Compiler ops
    /// @{
    CODE(":",       compile = def_word(word()));
    IMMD(";",       add_w(EXIT); compile = false);
    CODE("exit",    UNNEST());                                  // early exit the colon word
    CODE("variable",def_word(word()); add_var(VAR));            // create a variable
    CODE("constant",                                            // create a constant
         def_word(word());                                      // create a new word on dictionary
         add_w(LIT); add_du(POP());                             // dovar (+parameter field)
         add_w(EXIT));
    IMMD("immediate", dict[-1].attr |= IMM_ATTR);
    /// @}
    /// @defgroup metacompiler
    /// @brief - dict is directly used, instead of shield by macros
    /// @{
    CODE("exec",   IU w = POP(); CALL(w));                      // execute word
    CODE("create", def_word(word()); add_var(VBRAN));           // bran + offset field
    IMMD("does>",  add_w(DOES));
    IMMD("to",                                                  // alter the value of a constant, i.e. 3 to x
         IU w = VM==QUERY ? find(word()) : POP();               // constant addr
         if (!w) return;
         if (compile) {
             add_w(LIT); add_du((DU)w);                         // save addr on stack
             add_w(find("to"));                                 // encode to opcode
         }
         else {
             *(DU*)(MEM(dict[w].pfa) + sizeof(IU)) = POP();     // update constant
         });
    IMMD("is",              // ' y is x                         // alias a word, i.e. ' y is x
         IU w = VM==QUERY ? find(word()) : POP();               // word addr
         if (!w) return;
         if (compile) {
             add_w(LIT); add_du((DU)w);                         // save addr on stack
             add_w(find("is"));
         }
         else {
             dict[POP()].xt = dict[w].xt;
         });
    ///
    /// be careful with memory access, especially BYTE because
    /// it could make access misaligned which slows the access speed by 2x
    ///
    CODE("@",                                                   // w -- n
         IU w = UINT(POP());
         PUSH(w < USER_AREA ? (DU)IGET(w) : CELL(w)));          // check user area
    CODE("!",     IU w = UINT(POP()); CELL(w) = POP(););        // n w --
    CODE(",",     DU n = POP(); add_du(n));                     // n -- , compile a cell
    CODE("n,",    IU i = UINT(POP()); add_iu(i));               // compile a 16-bit value
    CODE("cells", IU i = UINT(POP()); PUSH(i * sizeof(DU)));    // n -- n'
    CODE("allot",                                               // n --
         IU n = UINT(POP());                                    // number of bytes
         for (int i = 0; i < n; i+=sizeof(DU)) add_du(DU0));    // zero padding
    CODE("th",    IU n = POP(); top += n * sizeof(DU));         // w i -- w'
    CODE("+!",    IU w = UINT(POP()); CELL(w) += POP());        // n w --
    CODE("?",     IU w = UINT(POP()); fout << CELL(w) << " ");  // w --
    /// @}
    /// @defgroup Debug ops
    /// @{
    CODE("abort", top = -DU1; ss.clear(); rs.clear());          // clear ss, rs
    CODE("here",  PUSH(HERE));
    CODE("'",     IU w = find(word()); if (w) PUSH(w));
    CODE(".s",    ss_dump(true));
    CODE("depth", PUSH(ss.idx));
    CODE("r",     PUSH(rs.idx));
    CODE("words", words());
    CODE("see",
         IU w = find(word()); if (!w) return;
         fout << ": " << dict[w].name;
         if (IS_UDF(w)) see(dict[w].pfa);
         else           fout << " ( built-ins ) ;";
         fout << ENDL);
    CODE("dump",  U32 n = UINT(POP()); mem_dump(UINT(POP()), n));
    CODE("dict",  dict_dump());
    CODE("forget",
         IU w = find(word()); if (!w) return;                  // bail, if not found
         IU b = find("boot")+1;
         if (w > b) {                                          // clear to specified word
             pmem.clear(dict[w].pfa - STRLEN(dict[w].name));
             dict.clear(w);
         }
         else {                                                // clear to 'boot'
             pmem.clear(USER_AREA);
             dict.clear(b);
         }
    );
    /// @}
    /// @defgroup OS ops
    /// @{
    CODE("mstat", mem_stat());
    CODE("ms",    PUSH(millis()));
    CODE("rnd",   PUSH(RND()));             // generate random number
    CODE("delay", delay(UINT(POP())));
    CODE("included",                        // include external file
         POP();                             // string length, not used
         load((const char*)MEM(POP())));    // include external file
#if DO_WASM
    CODE("JS",    native_api());            // Javascript interface
#else  // !DO_WASM
    CODE("bye",   exit(0));
#endif // DO_WASM    
    /// @}
    CODE("boot",  dict.clear(find("boot") + 1); pmem.clear(sizeof(DU)));
}
///====================================================================
///
///> ForthVM - Outer interpreter
///
DU parse_number(const char *idiom, int *err) {
    int b = static_cast<int>(*base);
    switch (*idiom) {                        ///> base override
    case '%': b = 2;  idiom++; break;
    case '&':
    case '#': b = 10; idiom++; break;
    case '$': b = 16; idiom++; break;
    }
    char *p;
    *err = errno = 0;
#if USE_FLOAT
    DU n = (b==10)
        ? static_cast<DU>(strtof(idiom, &p))
        : static_cast<DU>(strtol(idiom, &p, b));
#else  // !USE_FLOAT
    DU n = static_cast<DU>(strtol(idiom, &p, b));
#endif // USE_FLOAT
    if (errno || *p != '\0') *err = 1;
    return n;
}

void forth_core(const char *idiom) {     ///> aka QUERY
    VM = QUERY;
    IU w = find(idiom);                  ///> * get token by searching through dict
    if (w) {                             ///> * word found?
        if (compile && !IS_IMM(w)) {     /// * in compile mode?
            add_w(w);                    /// * add to colon word
        }
        else CALL(w);                    /// * execute forth word
        return;
    }
    // try as a number
    int err = 0;
    DU  n   = parse_number(idiom, &err);
    if (err) {                           /// * not number
        fout << idiom << "? " << ENDL;   ///> display error prompt
        compile = false;                 ///> reset to interpreter mode
        VM      = STOP;                  ///> skip the entire input buffer
    }
    // is a number
    if (compile) {                       /// * a number in compile mode?
        add_w(LIT);                      ///> add to current word
        add_du(n);
    }
    else PUSH(n);                        ///> or, add value onto data stack
}
///====================================================================
///
/// Forth VM external command processor
///
void forth_init() {
    static bool init = false;
    if (init) return;                    ///> check dictionary initilized

    base = &IGET(HERE);                  ///< set pointer to base
    add_iu(10);                          ///< allocate space for base
    dflt = &IGET(HERE);                  ///< set pointer to dfmt
    add_iu(USE_FLOAT);
    
    for (int i=pmem.idx; i<USER_AREA; i+=sizeof(IU)) {
        add_iu(0xffff);                  /// * padding user area
    }
    dict_compile();                      ///> compile dictionary
}
int forth_vm(const char *line, void(*hook)(int, const char*)) {
    auto time_up = []() {                /// * time slice up
        static long t0 = 0;              /// * real-time support, 10ms = 100Hz
        long t1 = millis();              ///> check timing
        return (t1 >= t0) ? (t0 = t1 + t0, 1) : 0;
    };
    auto cb = [](int, const char *rst) { printf("%s", rst); };
    fout_cb = hook ? hook : cb;          ///< serial output hook up

    bool resume = (VM==HOLD || VM==IO);  ///< check VM resume status
    if (resume) IP = UINT(rs.pop());     /// * restore context
    else {                               ///> refresh buffer if not resuming
        fout.str("");                    /// * clean output buffer
        fin.clear();                     /// * clear input stream error bit if any
        fin.str(line);                   /// * reload user command into input stream
    }
    string idiom;
    rs_dump("forth_vm >>");
    while (resume || fin >> idiom) {     /// * parse a word
        printf(" idiom=%s\n", idiom.c_str());
        if (resume) nest();                    /// * resume task
        else        forth_core(idiom.c_str()); /// * send to Forth core
        resume = VM==HOLD;
        if (resume && time_up()) break;  ///> multi-threading support
    }
    bool yield = VM==HOLD || VM==IO;     /// * yield to other tasks
    
    if (yield)         rs.push(IP);      /// * save context
    else if (!compile) ss_dump();        /// * optionally display stack contents

    rs_dump("forth_vm <<");
    return yield;
}
#include <iostream>
void outer(istream &in) {
    string cmd;
    while (getline(in, cmd)) {                /// * read line-by-line
        while (forth_vm(cmd.c_str()));
    }
}
int  main(int ac, char* av[]) {
    forth_init();
    srand(time(0));
    
#if !DO_WASM
    cout << APP_VERSION << endl;
    outer(cin);
    cout << "done!" << endl;
#endif // DO_WASM
    
    return 0;
}

///====================================================================
///
/// WASM/Emscripten ccall interfaces
///
#if DO_WASM
extern "C" {
int  forth(int n, char *cmd) {
    return (n==1) ? (PUSH(cmd[0]), 0) : forth_vm(cmd);
}
int  vm_base()       { return *base;    }
int  vm_dflt()       { return *dflt;    }
int  vm_ss_idx()     { return ss.idx;   }
int  vm_dict_idx()   { return dict.idx; }
int  vm_mem_idx()    { return pmem.idx; }       // HERE
DU   *vm_ss()        { return &ss[0];   }
char *vm_dict(int i) { return (char*)dict[i].name; }
char *vm_mem()       { return (char*)&pmem[0]; }
}

///====================================================================
///> WASM platform specific code
///
///> System statistics - for heap, stack, external memory debugging
///
void key() {
    EM_ASM({ postMessage(['key', 1]); });       /// set keypress mode
}

void dict_dump() {
    fout << setbase(16) << setfill('0') << "XT0=" << Code::XT0 << ENDL;
    for (int i=0; i<dict.idx; i++) {
        Code &c = dict[i];
        fout << setfill('0') << setw(3) << i
             << "> attr=" << (c.attr & 0x3)
             << ", xt="   << setw(4) << (IS_UDF(i) ? c.pfa : c.xtoff())
             << ":"       << setw(8) << (UFP)c.xt
             << ", name=" << setw(8) << (UFP)c.name
             << " "       << c.name << ENDL;
    }
    fout << setbase(*base) << setfill(' ') << setw(-1);
}
void mem_stat() {
    fout << APP_VERSION
         << "\n  dict: " << dict.idx  << "/" << E4_DICT_SZ
         << "\n  ss  : " << ss.idx    << "/" << E4_SS_SZ << " (max " << ss.max
         << ")\n  rs  : " << rs.idx   << "/" << E4_RS_SZ << " (max " << rs.max
         << ")\n  mem : " << HERE     << "/" << E4_PMEM_SZ << endl;
}
EM_JS(void, js_call, (const char *ops), {
        const req = UTF8ToString(ops).split(/\\s+/);
        const wa  = wasmExports;
        const mem = wa.vm_mem();
        let msg = [Date.now()], tfr = [];       ///< t0 anchor for performance
        for (let i=0, n=req.length; i < n; i++) {
            if (req[i]=='p') {
                const a = new Float32Array(     ///< create a buffer ref
                    wa.memory.buffer,           /// * WASM ArrayBuffer
                    mem + (req[i+1]|0),         /// * pointer address
                    req[i+2]|0                  /// * length
                );
                i += 2;                         /// *  skip over addr, len
                const t = new Float64Array(a);  ///< create a transferable
                msg.push(t);                    /// * which speeds postMessage
                tfr.push(t.buffer);             /// * from 20ms => 5ms
            }
            else msg.push(req[i]);
        }
        postMessage(['js', msg], tfr);
});
///
///> Javascript Native Interface, before passing to js_call()
///
///  String substitude similar to printf
///    %d - integer
///    %f - float
///    %x - hex
///    %s - string
///    %p - pointer (memory block)
///
void native_api() {                        ///> ( n addr u -- )
    stringstream n;
    auto t2s = [&n](char c) {              ///< template to string
        n.str("");                         /// * clear stream
        switch (c) {
        case 'd': n << UINT(POP());                break;
        case 'f': n << (DU)POP();                  break;
        case 'x': n << "0x" << hex << UINT(POP()); break;
        case 's': POP(); n << (char*)MEM(POP());   break;  /// also handles raw stream
        case 'p':
            n << "p " << UINT(POP());
            n << ' '  << UINT(POP());              break;
        default : n << c << '?';                   break;
        }
        return n.str();
    };
    POP();                                 /// * strlen, not used
    pad.clear();                           /// * borrow PAD for string op
    pad.append((char*)MEM(POP()));         /// copy string on stack
    for (size_t i=pad.find_last_of('%');   ///> find % from back
         i!=string::npos;                  /// * until not found
         i=pad.find_last_of('%',i?i-1:0)) {
        if (i && pad[i-1]=='%') {          /// * double %%
            pad.replace(--i,1,"");         /// * drop one %
        }
        else pad.replace(i, 2, t2s(pad[i+1]));
    }
    js_call(pad.c_str());    /// * pass to Emscripten function above
}
///
///> External file loader
///
int  forth_include(const char *fn) {              ///> include with Javascript
    static int bidx = E4_PMEM_SZ;                 ///< read buffer index
    const  int rst  = EM_ASM_INT({
        const txt = sync_fetch($0);               ///< fetch file from server subdir
        if (!txt) return 0;
        
        const len = txt.length + 1;               ///< script + '\0'
        const adj = (len + 0x10) & ~0xf;          ///< adjusted length, 16-byte aligned
        const wa  = wasmExports;                  ///< WASM export block
        const adr = wa.vm_mem() + $1 - adj;       ///< memory address from rear
        const buf = new Uint8Array(wa.memory.buffer, adr, len);
        for (var i=0; i < len; i++) {
            buf[i] = txt.charCodeAt(i);
        }
        buf[len-1] = '\0';                        /// * \0 terminated str
        
        return adj;
        }, fn, bidx);
    if (rst==0) {
        fout << fn << " load failed!" << ENDL;    /// * fetch failed, bail
        return 0;
    }
    bidx -= rst;                                  /// * buffer allocated
    ///
    /// preserve I/O states, call VM, restore IO states
    ///
    void (*cb)(int, const char*) = fout_cb;       ///< keep output port
    string in; getline(fin, in);                  ///< keep input buffer
    fout << ENDL;                                 /// * flush output

#if 1
    struct strbuf : public streambuf {            ///> in-place buffer
        strbuf(char *s, size_t n) { setg(s, s, s+n); }
    }
    strbuf((char*)&pmem[bidx], rst);              ///< create in-place string buf
    istream ss(&strbuf);                          ///< attach string to stream
    outer(ss);
#else
    while (forth_vm((const char*)&pmem[bidx]));   /// * send entire script to VM
#endif

    bidx += rst;                                  /// * release buffer
    fout_cb = cb;                                 /// * restore output port
    fin.clear(); fin.str(in);                     /// * restore input
    
    return 0;
}
#endif // DO_WSAM
