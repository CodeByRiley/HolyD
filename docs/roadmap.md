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

switch/case/default, labeled break/continue, and goto with D semantics.

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
-ast , write an ASTPrint(node, indent) recursive function. Half a day. Worth
more now that the AST is a tagged union and each node kind prints its own
payload.
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
  "[array]".
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
Postfix/prefix ++/-- as expressions. They parse as statements only; `b =
a++` does not work.
Ternary: cond ? a : b.
Cast: cast(Type) expression only.
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
break/continue , represented by AST_BREAK and AST_CONTINUE with an optional
label. The bytecode already has BC_FLOW_JUMP/BC_FLOW_JUMP_IF_FALSE; add
loop-context tracking in the compiler.
do...while , represented by AST_DO_WHILE.
Labeled statements , outer: for(...) and break outer;, represented by
AST_LABEL wrapping the labeled statement. The current AST does not yet have
named control-flow nodes.
goto and labeled goto , represented by AST_GOTO. They are part of D's
statement grammar; implement them without framing them as HolyC compatibility.
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

It is a bootstrap backend and deliberately not a fast one. Control flow
lowers to C control flow and calls lower to direct C calls, but values stay
boxed HDValues and variables stay in the runtime Environment, so an add
still costs a call into HDBinary and a variable read still costs a walk of a
linked list. What that buys is that every semantic the VM has came across
unchanged, which is what lets tools/difftest.sh hold the two paths to
byte-identical stdout and exit status. Keep it green: it is the only thing
making the backend trustworthy.

Two changes make the output fast, in this order. Both are worth having on
their own terms, and both are shared with any later backend.

Name resolution. Replace the name-keyed Environment with frame slots
computed at compile time: locals become real C locals, globals become
statics. This speeds the VM up as well, and it is testable against difftest
before the backend changes at all. One constraint to decide deliberately
rather than by accident: HolyD scopes variables to the function, not the
block, so slots are per function with declarations hoisted, and hoisting
changes behaviour where a function reads a name before declaring it locally
while a global of that name exists.

The type system, sections 2 and 3 above. Once expressions carry a static
type, I64 + I64 emits a + b instead of a call, and a boxed value survives
only where the value really is dynamic. This is what makes the generated C
worth compiling rather than merely correct.

The remaining backend choices stay open, and are cheaper than they look,
because name resolution and typing are most of the work and are shared:

x86_64 assembly emitter: direct and small, but every runtime and ABI detail
becomes the compiler's , register allocation, Win64 versus SysV calling
convention, and DWARF written by hand.
LLVM through its C API: the largest host dependency, which is a real cost
for a project that otherwise needs only libc and three system libraries. In
exchange, mem2reg means deliberately naive emission optimises itself, and
one IR reaches several architectures. It does not solve C ABI struct
passing; that layer stays in the frontend.

Section 2 will move the backend when it lands: retiring AST_INDEX_ASSIGN,
replacing AST_ARRAY_LEN_EXPR with AST_MEMBER_ACCESS, introducing
AST_ARRAY_LITERAL in place of an AST_CALL named "[array]", and giving
AST_CALL a callee expression all touch src/emit_c.c directly.

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
