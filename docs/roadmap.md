# HolyD roadmap

## Language direction

HolyD should relate to D the way HolyC relates to C: recognisably D in
syntax and semantics, but smaller, direct, systems-oriented, and integrated
with its own runtime.

D is the language of record. When D and C/HolyC differ, use the D form
unless a HolyD extension is explicitly documented. Do not support two
spellings for the same construct merely for C or HolyC compatibility.
Existing TempleOS-style scalar names such as I64 and U0 may remain HolyD
vocabulary; they do not change D's type-first declaration grammar.

Core syntax decisions:

- Type constructors belong with the type: I64* p, I64[10] fixed,
  I64[] dynamic, and U0 function(I64) callback.
- Casts use cast(Type) expression only. Do not add C-style (Type) expression.
- Struct and union pointers use D's automatic dot dereference: p.field.
  Do not add the C/C++ -> operator.
- Calls remain calls. Formatting is Print("Value: %d\n", value);, not a
  HolyC string-literal statement or a comma-expression trick.
- Conditional compilation uses version, debug, static if, and static assert.
  Do not add a textual C preprocessor.
- HolyD-specific facilities should be additive library/runtime features,
  not alternative grammar for constructs D already has.

## Priorities

Classes with D-style constructors and inheritance, for example
class Foo : Bar { I64 x; this(I64 value) { x = value; } }.

Pointers, static arrays, dynamic arrays, slices, bounds checks, and pointer
arithmetic. Explicit memory access remains load-bearing for a systems
language, while slices should be preferred when a length is known.

switch/case/default with D semantics. goto and labels are in, and so are
unlabelled break and continue; the labelled forms wait on labelled
statements.

Logical, bitwise, shift, power, concatenation, assignment, and conditional
operators sufficient for expression completeness.

D-style formatted output through an explicit function call.

D conditional compilation and module imports.

Memory primitives such as MAlloc, Free, CAlloc, and MSet as HolyD runtime
facilities. Whether HolyD has a GC is a runtime/profile decision, not a
reason to change D syntax.

extern(C) and other explicit linkage attributes for ABI interop.

## D surface to carry forward

- cast(I64) x, with no C-style cast alternative.
- ref and out parameters.
- pure, nothrow, @safe, and @nogc as enforceable semantic contracts, not
  cosmetic keywords.
- foreach (i, value; array), including the indexed form.
- array[1 .. 3] slicing and array[$ - 1] indexing.
- string as the D alias immutable(char)[], with string literals immutable.
- is expressions and assert(expression).
- scope(exit), scope(success), and scope(failure) cleanup guards.
- alias Name = Type;.
- unittest { ... } blocks, integrated with --test.
- version(NAME), debug(NAME), and static if conditions.
- __FILE__, __LINE__, __MODULE__, and related compile-time identifiers.

## 1. CLI and debugging

Wire up the missing flags the front-end can already support.

--help , done.
-tokens , done.
--emit-c and -o , done; see section 10.
-ast , done. ASTPrint(node, indent) recursively prints each node payload and
its parsed type syntax.
--dump-types , done. It prints resolved symbol types and the inferred type of
each AST node alongside its source span.
-version=NAME and -debug[=NAME] , feed D's conditional-compilation system.
Do not retain -D as a second spelling for the same feature.
This is cheap, unblocks debugging, and gives HolyD a D-shaped build model.

## 2. AST and type foundation

Refactor the AST before adding the rest of the language surface. ASTNode
should be a tagged union with a source span, rather than one structure whose
fields are reused by unrelated node kinds. Keep parsed type syntax in a
separate TypeSyntax tree instead of storing a single TokenType on declarations.

TypeSyntax needs named, pointer, fixed-array, dynamic-array, associative-array,
function, delegate, and typeof forms. Qualifiers such as const and immutable
belong on the type syntax. Fixed-array lengths remain expressions so they can
be evaluated during semantic analysis.

Clean up the existing expression representation:

- Add AST_BOOL_LITERAL, AST_CHAR_LITERAL, and AST_NULL_LITERAL.
- Add AST_ARRAY_LITERAL instead of representing an array as an AST_CALL named
  "[array]". Done.
- Add AST_UNARY_OP, AST_UPDATE, AST_CONDITIONAL, AST_CAST,
  AST_MEMBER_ACCESS, AST_SLICE, AST_DOLLAR, AST_NEW, and
  AST_FUNCTION_LITERAL.
- Add AST_ASSERT for runtime assertions and AST_IS_EXPRESSION for D's
  compile-time type-query grammar. Identity is remains an ordinary binary
  operator and is not the same construct as an is expression.
- Make AST_CALL contain a callee expression, not only a name, so the same node
  handles ordinary functions, methods, function pointers, and delegates.
- Make AST_ASSIGN contain a target, value, and assignment operator. Retire
  AST_INDEX_ASSIGN; indexing, member access, and pointer dereference are all
  ordinary assignment targets.
- Replace AST_ARRAY_LEN_EXPR with general AST_MEMBER_ACCESS. The semantic layer
  resolves built-in properties such as .length, .ptr, .dup, and .idup.
- Add AST_EXPR_STATEMENT so discarded expression values and D's restricted
  comma expressions are represented explicitly.

Add node kinds alongside their parser and semantic behavior, rather than
adding unused enum entries in advance.

## 3. Expression completeness

Done: logical &&, || and ! with short-circuit evaluation; bitwise & | ^ and
the shifts << >> >>>; % and ^^; and every compound assignment, which the
parser desugars to `a = a <op> b`. tests/operators.hd covers all of them,
including short-circuiting made observable rather than merely asserted.

The precedence chain in the parser now runs || && | ^ & ==/!= relational
shift additive multiplicative unary ^^ postfix. One deviation from D worth
settling: equality and relational sit on separate rungs here, as in C, so
`a < b < c` chains rather than being rejected. D puts them on one
non-associative level.

Still missing:

Bitwise complement ~x. The token is TOKEN_TILDE, which already means
concatenation as a binary operator, so this needs the unary/binary
distinction the parser now has for minus.
Postfix and prefix ++/-- currently lower as statements only; `++a;` and
`a++;` work, but `b = ++a` and `b = a++` do not yet preserve an expression
value.
Done: ternary `cond ? a : b`, including right associativity and lazy branch
evaluation on the VM, interpreter, and C backend.
Done: `cast(Type) expression` for Bool, integer, and F64 targets. Integer
casts truncate toward zero; all integer spellings currently share the
runtime's signed 64-bit storage. Pointer, string, array, and class-reference
casts still need explicit semantic rules.
Identity and membership: is, !is, in, and !in.
D is expressions use AST_IS_EXPRESSION; is and !is identity operations stay
in AST_BINARY_OP with the other binary operators.
Comma expressions follow D: each operand must have a side effect and the
result cannot be consumed. They are useful in expression statements and
for-loop increments, never for implicit printing.

Two semantics chosen while implementing the above, worth revisiting rather
than inheriting by accident:

^^ takes a non-negative integer exponent and is computed by repeated
multiplication. Using pow() would put libm in the link line of every
transpiled program, which the standalone build otherwise avoids entirely.
A fractional or negative exponent is refused rather than truncated.

Bitwise and shift operators are integer-only, and a shift of less than 0 or
more than 63 is refused rather than passed to the hardware, where C leaves
it undefined.

## 4. Statements and control flow

switch/case/default , represented by AST_SWITCH, AST_CASE, and AST_DEFAULT,
with break semantics. Add BC_FLOW_SWITCH/BC_FLOW_TABLE_SWITCH or lower to
if-else chains in codegen.
break/continue are done, unlabelled. AST_BREAK and AST_CONTINUE carry no
payload, and each backend tracks its own loop context: the VM records the
jumps and patches them when the target is emitted, the C emitter writes C's
break, and the assembler jumps to the loop's labels.

Continue is the half worth stating. All three lowerings put something after
the body , a for's increment, a foreach's counter bump , so a continue that
went straight to the test would leave the loop variable alone and spin. It
lands in front of that instead. In C that means a label and a goto rather
than C's continue, because the C a for lowers to is a while with the
increment at the end of its body; the label is only emitted when a continue
in that body actually needs it, since an unreferenced label is a warning
under the -Wall the generated C is compiled with.

Which loop a break belongs to is settled once, in src/resolve.c, rather than
three times: walk_loops rejects one outside a loop, so the VM, both backends
and --interpret refuse the same programs. A for's initialiser and increment
are expression positions in the parser, so neither statement can be written
there; if they ever become statement positions, which loop they bind to has
to be decided in walk_loops first, because the three backends put them in
three different places relative to the loop they lower to.

The tree walker refuses both, as it refuses goto and for the same reason: it
runs the AST by recursion and has no way to unwind out of it.

tests/break_continue.hd covers all three loop forms, nesting, a loop inside
a function, an unbounded while, and a trailing continue, on both backends.
do...while , represented by AST_DO_WHILE.
goto and labels are done. A label is declared `.name:` and jumped to with
`goto name;`; AST_LABEL and AST_GOTO carry the name, and src/resolve.c owns
which point in which function it means. Both backends consume that: the VM
patches BC_FLOW_JUMP addresses per chunk, and the C emitter writes a real C
goto. A jump may land anywhere in its own function, including inside a loop
body, and may not cross into a foreach body from outside it , that lowering
declares a loop counter on entry, and jumping past it would leave the loop
reading a counter nothing set. Nothing else can be skipped: every variable
is a frame slot with its own bound bit, so jumping past a declaration leaves
the name unbound rather than holding a stale value, identically on both
paths. `--interpret` refuses goto outright.
Labeled statements , outer: for(...) and break outer;. This is the remaining
half: AST_LABEL exists but wraps nothing, and break/continue do not yet take
a label to name.
scope(exit), scope(success), and scope(failure) use AST_SCOPE_GUARD with a
guard kind and body.

## 5. D conditional compilation

version (X) { ... }, debug { ... }, static if (cond) { ... }. Represent these
as AST_VERSION_BLOCK, AST_DEBUG_BLOCK, and AST_STATIC_IF and evaluate them in
the semantic layer.
Add AST_STATIC_ASSERT for compile-time failures. Unsatisfied branches must
still parse but do not need to pass semantic analysis.
There is no #define/#ifdef/#include mode. Source composition happens through
modules and import; build variants come from version and debug identifiers.

## 6. Structs, unions, enums, and classes

This is where HolyD's type and layout system becomes real.

struct , AST_STRUCT_DECL value type with fields. Memory layout, alignment,
offset computation.
union , AST_UNION_DECL with overlapping fields.
enum , AST_ENUM_DECL containing AST_ENUM_MEMBER declarations, optionally with
a base type.
Member access , obj.field. Add AST_MEMBER_ACCESS node, . operator in parser, BC_AGGREGATE_LOAD_FIELD/BC_AGGREGATE_STORE_FIELD with field offsets.
class with methods , AST_CLASS_DECL reference type, inheritance,
AST_INTERFACE_DECL interfaces, new, this(...)
constructors, ~this() destructors, and method dispatch. Do not use a
C++-style constructor named after the class. Define destruction and
allocation policy explicitly instead of adding C++ syntax by habit.
This is the phase where you'll want to start thinking hard about your type system and symbol table , you can't do member access without knowing struct layouts.

Current first executable slice: the C backend lowers `AST_CLASS_DECL` into a
heap-allocated C struct, embeds a base struct for inheritance, and emits
constructor factories plus receiver methods. It supports scalar and class
fields, `this.field`, `object.field`, method calls, both `this(...)` and
class-name constructor spelling, and inherited field/method lookup. A derived
class with no constructor currently forwards matching arguments to its base;
that is intentionally a bridge until explicit `super(...)` semantics exist.
The bytecode, assembly, direct-PE, and interpreter backends remain class-free
and reject class programs clearly.

## 7. Pointers and arrays

Pointer types , I64* p, U8* bytes, and pointers to aggregates.
Address-of & and dereference * , &x, *p.
Member access through a struct/union pointer uses p.field; D automatically
dereferences the pointer for dot access. Explicit (*p).field follows from
ordinary dereference and member access. Do not implement ->.
Pointer arithmetic , p + 1 where p is I64* should advance by sizeof(I64).
D-style arrays only:

- I64[10] fixed is a fixed-size value type.
- I64[] dynamic is a slice containing a pointer and length.
- array[i], array.length, array.ptr, array[a .. b], and array[$ - 1].
- Slicing is non-copying and produces a dynamic array.
- Indexing and slicing are bounds checked unless an explicitly unsafe build
  mode disables the checks.
- ~ concatenates arrays and ~= appends.
- .dup and .idup provide mutable and immutable copies.
- Value[Key] map adds associative arrays in a later milestone.
- Associative-array literals use AST_ASSOC_ARRAY_LITERAL rather than
  overloading AST_ARRAY_LITERAL with key/value pairs.
- Do not support C-style I64 array[10] declarations.

MAlloc, Free, CAlloc, and MSet may coexist as low-level runtime functions.
Prefer slices over naked pointers whenever the length is known.

## 8. Functions and modules

import resolution , AST_IMPORT_DECL nodes whose imported modules are parsed
and made available to semantic analysis.
module declaration , AST_MODULE_DECL namespacing the current file's symbols.
Default arguments , U0 Foo(I64 x = 5).
Default linkage is extern(D). Add extern(C) and extern(Windows) only for ABI
boundaries.
ref/out parameters , pass by reference with D semantics.
D-style typesafe variadics , for example U0 Log(string[] parts...).
Reserve raw ... handling for the D runtime model or explicitly declared
foreign ABIs; do not silently give ordinary HolyD functions C varargs.
Function pointers use U0 function(I64) fp and are called with fp(value).
Delegates use U0 delegate(I64) callback and carry a context pointer.
Add auto, typeof, and AST_FUNCTION_LITERAL before advanced templates.
alias declarations use AST_ALIAS_DECL. Function parameters should be a
dedicated parameter structure containing type syntax, storage class, default
value, and variadic information rather than parallel name arrays.
unittest declarations use AST_UNITTEST and remain distinct from ordinary
blocks so --test can discover them without relying on naming conventions.

## 9. Strings and formatted output

string is immutable(char)[], not an unrelated nominal token type. Mutable
text is char[]; .dup and .idup make the conversion explicit.

Use an ordinary call:

    Print("Value: %d, Name: %s\n", value, name);

Do not parse a bare string-literal/comma-list statement as an implicit call.
That is a HolyC surface feature, not a D one.

Implement format-specifier parsing (%d, %s, %x, %c, %.3f, etc.) behind the
Print API and wire it to the VM's I/O. Keep ~ and ~= as D's concatenation
operators for strings and other arrays.

## 10. Native code backend

The C transpiler is in. `--emit-c` writes a translation unit that links
against src/runtime.c and produces a standalone executable; see README,
"Compiling to an executable".

So is a code generator. `--emit-asm` writes x86-64 in GNU assembler syntax
for the Win64 ABI, links the same runtime, and is held to the VM by
`make difftest-asm` exactly as the C backend is. It is the deliberate step
before emitting machine code: instruction selection, frame layout and the
struct-passing rules are all in it and all checked, so a direct `--emit-exe`
after it is an instruction encoder and a PE or ELF writer bolted to a code
generator that already works, rather than all three at once.

The first `--emit-exe` delivery is intentionally a compiler driver: it emits
the tested Win64 assembly to a private temporary file and invokes a
GCC-compatible host assembler/linker, producing an executable in one Holyd
command. A direct PE writer remains the next native-code milestone; it must
replace the host toolchain, not merely hide it behind a different flag.

That direct work now starts as `--emit-pe`: it writes a PE32+ image itself,
emits the x64 process entry bytes, and resolves `KERNEL32!ExitProcess` through
its own import table. The deliberately narrow initial language is a
parameterless entry function returning a constant integer expression. The
next increments are native arithmetic/locals, control flow, and then the
runtime services currently supplied by C.

Two facts made it small. HDValue is 56 bytes, and both Win64 and SysV pass
anything over 16 bytes in memory and return it through a hidden pointer, so
every value travels as an address and the register classification smaller
structs would need never arises , that is the C ABI struct-passing layer
this section used to warn about, and for this language it lands in the easy
case. And the resolver had already turned every name into a frame slot and
every label into a jump target, which is most of what a code generator needs
before it can emit anything.

What it does not buy is speed. Values are still boxed and operators are
still runtime calls, so it emits the same calls the C backend emits and gcc
-O2 beats it by keeping things in registers around them. That ordering is
the point of the next paragraph.

It is a bootstrap backend and deliberately not a fast one. Control flow
lowers to C control flow and calls lower to direct C calls, but values stay
boxed HDValues, so an add still costs a call into HDBinary. What that buys
is that every semantic the VM has came across unchanged, which is what lets
tools/difftest.sh hold the two paths to byte-identical stdout and exit
status. Keep it green: it is the only thing making the backend trustworthy.

Name resolution is done. src/resolve.c gives every declaration and every use
a stable program-wide symbol identity and a slot in the global frame or in
its function frame, and both backends consume those slots: the VM has
LOCAL_LOAD/DEFINE/STORE and GLOBAL_LOAD/DEFINE/STORE over per-activation
frames, and the C emitter writes a C local or a file-scope static per slot.
Neither reads a variable out of the name-keyed Environment any more.

What the Environment is still for is the part that could not become a slot.
HolyD makes a declaration active where it is written, not at the top of its
frame, so until a declaration runs the name still means whatever the
enclosing scope means by it. Each slot therefore carries a bound bit, and
each symbol carries the next link in the search , a local falls back to a
global of the same name, a global falls back to the Environment, which is
where the FFI constants live and where an undefined name finally becomes an
error. Hoisting the declarations would have removed the Environment
entirely and quietly changed what a program means; tests/resolution.hd is
the difference, on both paths.

`--dump-symbols` prints the whole table: symbols with their storage, slot
and fallback, and every binding. `--dump-bytecode` shows the slot operands.
The first semantic type pass now lowers TypeSyntax to stable, interned type
IDs and records symbol and expression types in a side table; `--dump-types`
shows both. It is intentionally non-rejecting while the boxed runtime still
accepts compatibility cases such as assigning an integer to a pointer slot.

The C backend now consumes those types for representation-stable scalar
slots and expressions. I64/F64 locals and globals can become long long/double,
and safe arithmetic and comparisons emit C operators instead of HDBinaryX.
A pre-pass keeps a slot boxed if any declaration or write can carry a different
runtime representation; mixed I64/F64 ternaries are the important example.
The next step is typed function ABIs, followed by checked direct division,
remainder, shifts, and power.

The remaining backend choices stay open, and are cheaper than they look,
because name resolution is already done and shared, and typing will be:

x86_64 assembly emitter: direct and small, but every runtime and ABI detail
becomes the compiler's , register allocation, Win64 versus SysV calling
convention, and DWARF written by hand.
LLVM through its C API: the largest host dependency, which is a real cost
for a project that otherwise needs only libc and three system libraries. In
exchange, mem2reg means deliberately naive emission optimises itself, and
one IR reaches several architectures. It does not solve C ABI struct
passing; that layer stays in the frontend.

The AST_ARRAY_LITERAL migration already updated every backend. The remaining
section 2 cleanup will move them again: retiring AST_INDEX_ASSIGN, replacing
AST_ARRAY_LEN_EXPR with AST_MEMBER_ACCESS, and giving AST_CALL a callee
expression all touch src/emit_c.c directly.

## Standard library

Build this after the majority of language features are implemented and
tested.

File I/O , ReadFile, WriteFile, etc.
Memory management , MAlloc, Free, etc.
Math , Sin, Cos, etc.
String manipulation , StrLen, StrCpy, etc.
Collections , dynamic arrays and associative arrays first, then List, Map,
and higher-level containers where they add value.

The standard library may be deliberately smaller and more direct than
Phobos. That is an appropriate HolyD simplification; changing D grammar is
not.
