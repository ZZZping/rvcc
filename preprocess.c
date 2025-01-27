// This file implements the C preprocessor.
//
// The preprocessor takes a list of tokens as an input and returns a
// new list of tokens as an output.
//
// The preprocessing language is designed in such a way that that's
// guaranteed to stop even if there is a recursive macro.
// Informally speaking, a macro is applied only once for each token.
// That is, if a macro token T appears in a result of direct or
// indirect macro expansion of T, T won't be expanded any further.
// For example, if T is defined as U, and U is defined as T, then
// token T is expanded to U and then to T and the macro expansion
// stops at that point.
//
// To achieve the above behavior, we attach for each token a set of
// macro names from which the token is expanded. The set is called
// "hideset". Hideset is initially empty, and every time we expand a
// macro, the macro name is added to the resulting tokens' hidesets.
//
// The above macro expansion algorithm is explained in this document
// written by Dave Prossor, which is used as a basis for the
// standard's wording:
// https://github.com/rui314/chibicc/wiki/cpp.algo.pdf

#include "rvcc.h"

typedef struct MacroParam MacroParam;
struct MacroParam {
  MacroParam *Next;
  char *Name;
};

typedef struct MacroArg MacroArg;
struct MacroArg {
  MacroArg *Next;
  char *Name;
  Token *Tok;
};

typedef Token *macroHandlerFn(Token *);

typedef struct Macro Macro;
struct Macro {
  Macro *Next;
  char *Name;
  bool IsObjlike; // Object-like or function-like
  MacroParam *Params;
  bool IsVariadic;
  Token *Body;
  bool Deleted;
  macroHandlerFn *Handler;
};

// `#if` can be nested, so we use a stack to manage nested `#if`s.
typedef struct CondIncl CondIncl;
struct CondIncl {
  CondIncl *Next;
  enum { IN_THEN, IN_ELIF, IN_ELSE } Ctx;
  Token *Tok;
  bool Included;
};

typedef struct Hideset Hideset;
struct Hideset {
  Hideset *Next;
  char *Name;
};

static Macro *Macros;
static CondIncl *CondInclude;

static Token *preprocess2(Token *Tok);
static Macro *findMacro(Token *tok);

static bool isHash(Token *Tok) { return Tok->AtBOL && equal(Tok, "#"); }

// Some preprocessor directives such as #include allow extraneous
// tokens before newline. This function skips such tokens.
static Token *skipLine(Token *Tok) {
  if (Tok->AtBOL)
    return Tok;
  warnTok(Tok, "extra token");
  while (Tok->AtBOL)
    Tok = Tok->Next;
  return Tok;
}

static Token *copyToken(Token *Tok) {
  Token *T = calloc(1, sizeof(Token));
  *T = *Tok;
  T->Next = NULL;
  return T;
}

static Token *newEOF(Token *Tok) {
  Token *T = copyToken(Tok);
  T->Kind = TK_EOF;
  T->Len = 0;
  return T;
}

static Hideset *newHideset(char *Name) {
  Hideset *Hs = calloc(1, sizeof(Hideset));
  Hs->Name = Name;
  return Hs;
}

static Hideset *hidesetUnion(Hideset *Hs1, Hideset *Hs2) {
  Hideset Head = {};
  Hideset *Cur = &Head;

  for (; Hs1; Hs1 = Hs1->Next)
    Cur = Cur->Next = newHideset(Hs1->Name);
  Cur->Next = Hs2;
  return Head.Next;
}

static bool hidesetContains(Hideset *Hs, char *S, int Len) {
  for (; Hs; Hs = Hs->Next)
    if (strlen(Hs->Name) == Len && !strncmp(Hs->Name, S, Len))
      return true;
  return false;
}

static Hideset *hidesetIntersection(Hideset *Hs1, Hideset *Hs2) {
  Hideset Head = {};
  Hideset *Cur = &Head;

  for (; Hs1; Hs1 = Hs1->Next)
    if (hidesetContains(Hs2, Hs1->Name, strlen(Hs1->Name)))
      Cur = Cur->Next = newHideset(Hs1->Name);
  return Head.Next;
}

static Token *addHideset(Token *Tok, Hideset *Hs) {
  Token Head = {};
  Token *Cur = &Head;

  for (; Tok; Tok = Tok->Next) {
    Token *T = copyToken(Tok);
    T->Hideset = hidesetUnion(T->Hideset, Hs);
    Cur = Cur->Next = T;
  }
  return Head.Next;
}

// Append tok2 to the end of tok1.
static Token *append(Token *Tok1, Token *Tok2) {
  if (Tok1->Kind == TK_EOF)
    return Tok2;

  Token Head = {};
  Token *Cur = &Head;

  for (; Tok1->Kind != TK_EOF; Tok1 = Tok1->Next)
    Cur = Cur->Next = copyToken(Tok1);
  Cur->Next = Tok2;
  return Head.Next;
}

static Token *skipCondIncl2(Token *Tok) {
  while (Tok->Kind != TK_EOF) {
    if (isHash(Tok) && (equal(Tok->Next, "if") || equal(Tok->Next, "ifdef") ||
                        equal(Tok->Next, "ifndef"))) {
      Tok = skipCondIncl2(Tok->Next->Next);
      continue;
    }
    if (isHash(Tok) && equal(Tok->Next, "endif"))
      return Tok->Next->Next;
    Tok = Tok->Next;
  }
  return Tok;
}

// Skip until next `#else`, `#elif` or `#endif`.
// Nested `#if` and `#endif` are skipped.
static Token *skipCondIncl(Token *Tok) {
  while (Tok->Kind != TK_EOF) {
    if (isHash(Tok) && (equal(Tok->Next, "if") || equal(Tok->Next, "ifdef") ||
                        equal(Tok->Next, "ifndef"))) {
      Tok = skipCondIncl2(Tok->Next->Next);
      continue;
    }

    if (isHash(Tok) && (equal(Tok->Next, "elif") || equal(Tok->Next, "else") ||
                        equal(Tok->Next, "endif")))
      break;
    Tok = Tok->Next;
  }
  return Tok;
}

// Double-quote a given string and returns it.
static char *quoteString(char *Str) {
  int BufSize = 3;
  for (int I = 0; Str[I]; I++) {
    if (Str[I] == '\\' || Str[I] == '"')
      BufSize++;
    BufSize++;
  }

  char *Buf = calloc(1, BufSize);
  char *P = Buf;
  *P++ = '"';
  for (int I = 0; Str[I]; I++) {
    if (Str[I] == '\\' || Str[I] == '"')
      *P++ = '\\';
    *P++ = Str[I];
  }
  *P++ = '"';
  *P++ = '\0';
  return Buf;
}

static Token *newStrToken(char *Str, Token *Tmpl) {
  char *Buf = quoteString(Str);
  return tokenize(newFile(Tmpl->File->Name, Tmpl->File->FileNo, Buf));
}

// Copy all tokens until the next newline, terminate them with
// an EOF token and then returns them. This function is used to
// create a new list of tokens for `#if` arguments.
static Token *copyLine(Token **Rest, Token *Tok) {
  Token head = {};
  Token *Cur = &head;

  for (; !Tok->AtBOL; Tok = Tok->Next)
    Cur = Cur->Next = copyToken(Tok);

  Cur->Next = newEOF(Tok);
  *Rest = Tok;
  return head.Next;
}

static Token *newNumToken(int Val, Token *Tmpl) {
  char *Buf = format("%d\n", Val);
  return tokenize(newFile(Tmpl->File->Name, Tmpl->File->FileNo, Buf));
}

static Token *readConstExpr(Token **rest, Token *tok) {
  tok = copyLine(rest, tok);

  Token head = {};
  Token *cur = &head;

  while (tok->Kind != TK_EOF) {
    // "defined(foo)" or "defined foo" becomes "1" if macro "foo"
    // is defined. Otherwise "0".
    if (equal(tok, "defined")) {
      Token *start = tok;
      bool has_paren = consume(&tok, tok->Next, "(");

      if (tok->Kind != TK_IDENT)
        errorTok(start, "macro name must be an identifier");
      Macro *m = findMacro(tok);
      tok = tok->Next;

      if (has_paren)
        tok = skip(tok, ")");

      cur = cur->Next = newNumToken(m ? 1 : 0, start);
      continue;
    }

    cur = cur->Next = tok;
    tok = tok->Next;
  }

  cur->Next = tok;
  return head.Next;
}

// Read and evaluate a constant expression.
static long evalConstExpr(Token **Rest, Token *Tok) {
  Token *Start = Tok;
  Token *Expr = readConstExpr(Rest, Tok->Next);
  Expr = preprocess2(Expr);

  if (Expr->Kind == TK_EOF)
    errorTok(Start, "no expression");

  // [https://www.sigbus.info/n1570#6.10.1p4] The standard requires
  // we replace remaining non-macro identifiers with "0" before
  // evaluating a constant expression. For example, `#if foo` is
  // equivalent to `#if 0` if foo is not defined.
  for (Token *T = Expr; T->Kind != TK_EOF; T = T->Next) {
    if (T->Kind == TK_IDENT) {
      Token *Next = T->Next;
      *T = *newNumToken(0, T);
      T->Next = Next;
    }
  }

  Token *Rest2;
  long Val = constExpr(&Rest2, Expr);
  if (Rest2->Kind != TK_EOF)
    errorTok(Rest2, "extra token");
  return Val;
}

static CondIncl *pushCondIncl(Token *Tok, bool Included) {
  CondIncl *CI = calloc(1, sizeof(CondIncl));
  CI->Next = CondInclude;
  CI->Ctx = IN_THEN;
  CI->Tok = Tok;
  CI->Included = Included;
  CondInclude = CI;
  return CI;
}

static Macro *findMacro(Token *Tok) {
  if (Tok->Kind != TK_IDENT)
    return NULL;

  for (Macro *M = Macros; M; M = M->Next)
    if (strlen(M->Name) == Tok->Len && !strncmp(M->Name, Tok->Loc, Tok->Len))
      return M->Deleted ? NULL : M;
  return NULL;
}

static Macro *addMacro(char *Name, bool IsObjlike, Token *Body) {
  Macro *M = calloc(1, sizeof(Macro));
  M->Next = Macros;
  M->Name = Name;
  M->IsObjlike = IsObjlike;
  M->Body = Body;
  Macros = M;
  return M;
}

static MacroParam *readMacroParams(Token **Rest, Token *Tok, bool *IsVariadic) {
  MacroParam Head = {};
  MacroParam *Cur = &Head;

  while (!equal(Tok, ")")) {
    if (Cur != &Head)
      Tok = skip(Tok, ",");

    if (equal(Tok, "...")) {
      *IsVariadic = true;
      *Rest = skip(Tok->Next, ")");
      return Head.Next;
    }

    if (Tok->Kind != TK_IDENT)
      errorTok(Tok, "expected an identifier");
    MacroParam *M = calloc(1, sizeof(MacroParam));
    M->Name = strndup(Tok->Loc, Tok->Len);
    Cur = Cur->Next = M;
    Tok = Tok->Next;
  }
  *Rest = Tok->Next;
  return Head.Next;
}

static void readMacroDefinition(Token **Rest, Token *Tok) {
  if (Tok->Kind != TK_IDENT)
    errorTok(Tok, "macro name must be an identifier");
  char *Name = strndup(Tok->Loc, Tok->Len);
  Tok = Tok->Next;

  if (!Tok->HasSpace && equal(Tok, "(")) {
    // Function-like macro
    bool IsVariadic = false;
    MacroParam *Params = readMacroParams(&Tok, Tok->Next, &IsVariadic);

    Macro *M = addMacro(Name, false, copyLine(Rest, Tok));
    M->Params = Params;
    M->IsVariadic = IsVariadic;
  } else {
    // Object-like macro
    addMacro(Name, true, copyLine(Rest, Tok));
  }
}

static MacroArg *readMacroArgOne(Token **Rest, Token *Tok, bool ReadRest) {
  Token Head = {};
  Token *Cur = &Head;
  int Level = 0;

  while (true) {
    if (Level == 0 && equal(Tok, ")"))
      break;
    if (Level == 0 && !ReadRest && equal(Tok, ","))
      break;

    if (Tok->Kind == TK_EOF)
      errorTok(Tok, "premature end of input");

    if (equal(Tok, "("))
      Level++;
    else if (equal(Tok, ")"))
      Level--;

    Cur = Cur->Next = copyToken(Tok);
    Tok = Tok->Next;
  }

  Cur->Next = newEOF(Tok);

  MacroArg *Arg = calloc(1, sizeof(MacroArg));
  Arg->Tok = Head.Next;
  *Rest = Tok;
  return Arg;
}

static MacroArg *readMacroArgs(Token **Rest, Token *Tok, MacroParam *Params,
                               bool IsVariadic) {
  Token *Start = Tok;
  Tok = Tok->Next->Next;

  MacroArg Head = {};
  MacroArg *Cur = &Head;

  MacroParam *PP = Params;
  for (; PP; PP = PP->Next) {
    if (Cur != &Head)
      Tok = skip(Tok, ",");
    Cur = Cur->Next = readMacroArgOne(&Tok, Tok, false);
    Cur->Name = PP->Name;
  }

  if (IsVariadic) {
    MacroArg *Arg;
    if (equal(Tok, ")")) {
      Arg = calloc(1, sizeof(MacroArg));
      Arg->Tok = newEOF(Tok);
    } else {
      if (PP != Params)
        Tok = skip(Tok, ",");
      Arg = readMacroArgOne(&Tok, Tok, true);
    }
    Arg->Name = "__VA_ARGS__";
    Cur = Cur->Next = Arg;
  } else if (PP) {
    errorTok(Start, "too many arguments");
  }

  skip(Tok, ")");
  *Rest = Tok;
  return Head.Next;
}

static MacroArg *findArg(MacroArg *Args, Token *Tok) {
  for (MacroArg *AP = Args; AP; AP = AP->Next)
    if (Tok->Len == strlen(AP->Name) && !strncmp(Tok->Loc, AP->Name, Tok->Len))
      return AP;
  return NULL;
}

// Concatenates all tokens in `tok` and returns a new string.
static char *joinTokens(Token *Tok, Token *End) {
  // Compute the length of the resulting token.
  int Len = 1;
  for (Token *T = Tok; T != End && T->Kind != TK_EOF; T = T->Next) {
    if (T != Tok && T->HasSpace)
      Len++;
    Len += T->Len;
  }

  char *Buf = calloc(1, Len);

  // Copy token texts.
  int Pos = 0;
  for (Token *T = Tok; T != End && T->Kind != TK_EOF; T = T->Next) {
    if (T != Tok && T->HasSpace)
      Buf[Pos++] = ' ';
    strncpy(Buf + Pos, T->Loc, T->Len);
    Pos += T->Len;
  }
  Buf[Pos] = '\0';
  return Buf;
}

// Concatenates all tokens in `arg` and returns a new string token.
// This function is used for the stringizing operator (#).
static Token *stringize(Token *Hash, Token *Arg) {
  // Create a new string token. We need to set some value to its
  // source location for error reporting function, so we use a macro
  // name token as a template.
  char *S = joinTokens(Arg, NULL);
  return newStrToken(S, Hash);
}

// Concatenate two tokens to create a new token.
static Token *paste(Token *LHS, Token *RHS) {
  // Paste the two tokens.
  char *Buf = format("%.*s%.*s", LHS->Len, LHS->Loc, RHS->Len, RHS->Loc);

  // Tokenize the resulting string.
  Token *Tok = tokenize(newFile(LHS->File->Name, LHS->File->FileNo, Buf));
  if (Tok->Next->Kind != TK_EOF)
    errorTok(LHS, "pasting forms '%s', an invalid token", Buf);
  return Tok;
}

// Replace func-like macro parameters with given arguments.
static Token *subst(Token *Tok, MacroArg *Args) {
  Token Head = {};
  Token *Cur = &Head;

  while (Tok->Kind != TK_EOF) {
    // "#" followed by a parameter is replaced with stringized actuals.
    if (equal(Tok, "#")) {
      MacroArg *Arg = findArg(Args, Tok->Next);
      if (!Arg)
        errorTok(Tok->Next, "'#' is not followed by a macro parameter");
      Cur = Cur->Next = stringize(Tok, Arg->Tok);
      Tok = Tok->Next->Next;
      continue;
    }

    if (equal(Tok, "##")) {
      if (Cur == &Head)
        errorTok(Tok, "'##' cannot appear at start of macro expansion");

      if (Tok->Next->Kind == TK_EOF)
        errorTok(Tok, "'##' cannot appear at end of macro expansion");

      MacroArg *Arg = findArg(Args, Tok->Next);
      if (Arg) {
        if (Arg->Tok->Kind != TK_EOF) {
          *Cur = *paste(Cur, Arg->Tok);
          for (Token *T = Arg->Tok->Next; T->Kind != TK_EOF; T = T->Next)
            Cur = Cur->Next = copyToken(T);
        }
        Tok = Tok->Next->Next;
        continue;
      }

      *Cur = *paste(Cur, Tok->Next);
      Tok = Tok->Next->Next;
      continue;
    }

    MacroArg *Arg = findArg(Args, Tok);

    if (Arg && equal(Tok->Next, "##")) {
      Token *RHS = Tok->Next->Next;

      if (Arg->Tok->Kind == TK_EOF) {
        MacroArg *Arg2 = findArg(Args, RHS);
        if (Arg2) {
          for (Token *T = Arg2->Tok; T->Kind != TK_EOF; T = T->Next)
            Cur = Cur->Next = copyToken(T);
        } else {
          Cur = Cur->Next = copyToken(RHS);
        }
        Tok = RHS->Next;
        continue;
      }

      for (Token *T = Arg->Tok; T->Kind != TK_EOF; T = T->Next)
        Cur = Cur->Next = copyToken(T);
      Tok = Tok->Next;
      continue;
    }

    // Handle a macro token. Macro arguments are completely macro-expanded
    // before they are substituted into a macro body.
    if (Arg) {
      Token *T = preprocess2(Arg->Tok);
      T->AtBOL = Tok->AtBOL;
      T->HasSpace = Tok->HasSpace;
      for (; T->Kind != TK_EOF; T = T->Next)
        Cur = Cur->Next = copyToken(T);
      Tok = Tok->Next;
      continue;
    }

    // Handle a non-macro token.
    Cur = Cur->Next = copyToken(Tok);
    Tok = Tok->Next;
    continue;
  }

  Cur->Next = Tok;
  return Head.Next;
}

// If tok is a macro, expand it and return true.
// Otherwise, do nothing and return false.
static bool expandMacro(Token **Rest, Token *Tok) {
  if (hidesetContains(Tok->Hideset, Tok->Loc, Tok->Len))
    return false;

  Macro *M = findMacro(Tok);
  if (!M)
    return false;

  // Built-in dynamic macro application such as __LINE__
  if (M->Handler) {
    *Rest = M->Handler(Tok);
    (*Rest)->Next = Tok->Next;
    return true;
  }

  // Object-like macro application
  if (M->IsObjlike) {
    Hideset *Hs = hidesetUnion(Tok->Hideset, newHideset(M->Name));
    Token *Body = addHideset(M->Body, Hs);
    for (Token *T = Body; T->Kind != TK_EOF; T = T->Next)
      T->Origin = Tok;
    *Rest = append(Body, Tok->Next);
    (*Rest)->AtBOL = Tok->AtBOL;
    (*Rest)->HasSpace = Tok->HasSpace;
    return true;
  }

  // If a funclike macro token is not followed by an argument list,
  // treat it as a normal identifier.
  if (!equal(Tok->Next, "("))
    return false;

  // Function-like macro application
  Token *MacroToken = Tok;
  MacroArg *Args = readMacroArgs(&Tok, Tok, M->Params, M->IsVariadic);
  Token *Rparen = Tok;

  // Tokens that consist a func-like macro invocation may have different
  // hidesets, and if that's the case, it's not clear what the hideset
  // for the new tokens should be. We take the interesection of the
  // macro token and the closing parenthesis and use it as a new hideset
  // as explained in the Dave Prossor's algorithm.
  Hideset *Hs = hidesetIntersection(MacroToken->Hideset, Rparen->Hideset);
  Hs = hidesetUnion(Hs, newHideset(M->Name));

  Token *Body = subst(M->Body, Args);
  Body = addHideset(Body, Hs);
  for (Token *T = Body; T->Kind != TK_EOF; T = T->Next)
    T->Origin = MacroToken;
  *Rest = append(Body, Tok->Next);
  (*Rest)->AtBOL = MacroToken->AtBOL;
  (*Rest)->HasSpace = MacroToken->HasSpace;
  return true;
}

static char *searchIncludePaths(char *Filename) {
  if (Filename[0] == '/')
    return Filename;

  // Search a file from the include paths.
  for (int I = 0; I < IncludePaths.Len; I++) {
    char *Path = format("%s/%s", IncludePaths.Data[I], Filename);
    if (fileExists(Path))
      return Path;
  }
  return NULL;
}

// Read an #include argument.
static char *readIncludeFilename(Token **Rest, Token *Tok, bool *IsDquote) {
  // Pattern 1: #include "foo.h"
  if (Tok->Kind == TK_STR) {
    // A double-quoted filename for #include is a special kind of
    // token, and we don't want to interpret any escape sequences in it.
    // For example, "\f" in "C:\foo" is not a formfeed character but
    // just two non-control characters, backslash and f.
    // So we don't want to use token->Str.
    *IsDquote = true;
    *Rest = skipLine(Tok->Next);
    return strndup(Tok->Loc + 1, Tok->Len - 2);
  }

  // Pattern 2: #include <foo.h>
  if (equal(Tok, "<")) {
    // Reconstruct a filename from a sequence of tokens between
    // "<" and ">".
    Token *Start = Tok;

    // Find closing ">".
    for (; !equal(Tok, ">"); Tok = Tok->Next)
      if (Tok->AtBOL || Tok->Kind == TK_EOF)
        errorTok(Tok, "expected '>'");

    *IsDquote = false;
    *Rest = skipLine(Tok->Next);
    return joinTokens(Start->Next, Tok);
  }

  // Pattern 3: #include FOO
  // In this case FOO must be macro-expanded to either
  // a single string token or a sequence of "<" ... ">".
  if (Tok->Kind == TK_IDENT) {
    Token *Tok2 = preprocess2(copyLine(Rest, Tok));
    return readIncludeFilename(&Tok2, Tok2, IsDquote);
  }

  errorTok(Tok, "expected a filename");
  return NULL;
}

static Token *includeFile(Token *Tok, char *Path, Token *FilenameTok) {
  Token *Tok2 = tokenizeFile(Path);
  if (!Tok2)
    errorTok(FilenameTok, "%s: cannot open file: %s", Path, strerror(errno));
  return append(Tok2, Tok);
}

// Visit all tokens in `tok` while evaluating preprocessing
// macros and directives.
static Token *preprocess2(Token *Tok) {
  Token Head = {};
  Token *Cur = &Head;

  while (Tok->Kind != TK_EOF) {
    // If it is a macro, expand it.
    if (expandMacro(&Tok, Tok))
      continue;

    // Pass through if it is not a "#".
    if (!isHash(Tok)) {
      Cur = Cur->Next = Tok;
      Tok = Tok->Next;
      continue;
    }

    Token *Start = Tok;
    Tok = Tok->Next;

    if (equal(Tok, "include")) {
      bool IsDquote;
      char *Filename = readIncludeFilename(&Tok, Tok->Next, &IsDquote);

      if (Filename[0] != '/' && IsDquote) {
        char *Path =
            format("%s/%s", dirname(strdup(Start->File->Name)), Filename);
        if (fileExists(Path)) {
          Tok = includeFile(Tok, Path, Start->Next->Next);
          continue;
        }
      }

      char *Path = searchIncludePaths(Filename);
      Tok = includeFile(Tok, Path ? Path : Filename, Start->Next->Next);
      continue;
    }

    if (equal(Tok, "define")) {
      readMacroDefinition(&Tok, Tok->Next);
      continue;
    }

    if (equal(Tok, "undef")) {
      Tok = Tok->Next;
      if (Tok->Kind != TK_IDENT)
        errorTok(Tok, "macro name must be an identifier");
      char *Name = strndup(Tok->Loc, Tok->Len);
      Tok = skipLine(Tok->Next);

      Macro *M = addMacro(Name, true, NULL);
      M->Deleted = true;
      continue;
    }

    if (equal(Tok, "if")) {
      long Val = evalConstExpr(&Tok, Tok);
      pushCondIncl(Start, Val);
      if (!Val)
        Tok = skipCondIncl(Tok);
      continue;
    }

    if (equal(Tok, "ifdef")) {
      bool Defined = findMacro(Tok->Next);
      pushCondIncl(Tok, Defined);
      Tok = skipLine(Tok->Next->Next);
      if (!Defined)
        Tok = skipCondIncl(Tok);
      continue;
    }

    if (equal(Tok, "ifndef")) {
      bool Defined = findMacro(Tok->Next);
      pushCondIncl(Tok, !Defined);
      Tok = skipLine(Tok->Next->Next);
      if (Defined)
        Tok = skipCondIncl(Tok);
      continue;
    }

    if (equal(Tok, "elif")) {
      if (!CondInclude || CondInclude->Ctx == IN_ELSE)
        errorTok(Start, "stray #elif");
      CondInclude->Ctx = IN_ELIF;

      if (!CondInclude->Included && evalConstExpr(&Tok, Tok))
        CondInclude->Included = true;
      else
        Tok = skipCondIncl(Tok);
      continue;
    }

    if (equal(Tok, "else")) {
      if (!CondInclude || CondInclude->Ctx == IN_ELSE)
        errorTok(Start, "stray #else");
      CondInclude->Ctx = IN_ELSE;
      Tok = skipLine(Tok->Next);

      if (CondInclude->Included)
        Tok = skipCondIncl(Tok);
      continue;
    }

    if (equal(Tok, "endif")) {
      if (!CondInclude)
        errorTok(Start, "stray #endif");
      CondInclude = CondInclude->Next;
      Tok = skipLine(Tok->Next);
      continue;
    }

    if (equal(Tok, "error"))
      errorTok(Tok, "error");

    // `#`-only line is legal. It's called a null directive.
    if (Tok->AtBOL)
      continue;

    errorTok(Tok, "invalid preprocessor directive");
  }

  Cur->Next = Tok;
  return Head.Next;
}

static void defineMacro(char *Name, char *Buf) {
  Token *Tok = tokenize(newFile("<built-in>", 1, Buf));
  addMacro(Name, true, Tok);
}

static Macro *addBuiltin(char *Name, macroHandlerFn *Fn) {
  Macro *M = addMacro(Name, true, NULL);
  M->Handler = Fn;
  return M;
}

static Token *fileMacro(Token *Tmpl) {
  while (Tmpl->Origin)
    Tmpl = Tmpl->Origin;
  return newStrToken(Tmpl->File->Name, Tmpl);
}

static Token *lineMacro(Token *Tmpl) {
  while (Tmpl->Origin)
    Tmpl = Tmpl->Origin;
  return newNumToken(Tmpl->LineNo, Tmpl);
}

static void initMacros(void) {
  // Define predefined macros
  defineMacro("_LP64", "1");
  defineMacro("__C99_MACRO_WITH_VA_ARGS", "1");
  defineMacro("__ELF__", "1");
  defineMacro("__LP64__", "1");
  defineMacro("__SIZEOF_DOUBLE__", "8");
  defineMacro("__SIZEOF_FLOAT__", "4");
  defineMacro("__SIZEOF_INT__", "4");
  defineMacro("__SIZEOF_LONG_DOUBLE__", "8");
  defineMacro("__SIZEOF_LONG_LONG__", "8");
  defineMacro("__SIZEOF_LONG__", "8");
  defineMacro("__SIZEOF_POINTER__", "8");
  defineMacro("__SIZEOF_PTRDIFF_T__", "8");
  defineMacro("__SIZEOF_SHORT__", "2");
  defineMacro("__SIZEOF_SIZE_T__", "8");
  defineMacro("__SIZE_TYPE__", "unsigned long");
  defineMacro("__STDC_HOSTED__", "1");
  defineMacro("__STDC_NO_ATOMICS__", "1");
  defineMacro("__STDC_NO_COMPLEX__", "1");
  defineMacro("__STDC_NO_THREADS__", "1");
  defineMacro("__STDC_NO_VLA__", "1");
  defineMacro("__STDC_VERSION__", "201112L");
  defineMacro("__STDC__", "1");
  defineMacro("__USER_LABEL_PREFIX__", "");
  defineMacro("__alignof__", "_Alignof");
  defineMacro("__rvcc__", "1");
  defineMacro("__const__", "const");
  defineMacro("__gnu_linux__", "1");
  defineMacro("__inline__", "inline");
  defineMacro("__linux", "1");
  defineMacro("__linux__", "1");
  defineMacro("__signed__", "signed");
  defineMacro("__typeof__", "typeof");
  defineMacro("__unix", "1");
  defineMacro("__unix__", "1");
  defineMacro("__volatile__", "volatile");
  defineMacro("linux", "1");
  defineMacro("unix", "1");
  defineMacro("__riscv_mul", "1");
  defineMacro("__riscv_muldiv", "1");
  defineMacro("__riscv_fdiv", "1");
  defineMacro("__riscv_xlen", "64");
  defineMacro("__riscv", "1");
  defineMacro("__riscv64", "1");
  defineMacro("__riscv_div", "1");
  defineMacro("__riscv_float_abi_double", "1");
  defineMacro("__riscv_flen", "64");

  addBuiltin("__FILE__", fileMacro);
  addBuiltin("__LINE__", lineMacro);
}

// Concatenate adjacent string literals into a single string literal
// as per the C spec.
static void joinAdjacentStringLiterals(Token *Tok1) {
  while (Tok1->Kind != TK_EOF) {
    if (Tok1->Kind != TK_STR || Tok1->Next->Kind != TK_STR) {
      Tok1 = Tok1->Next;
      continue;
    }

    Token *Tok2 = Tok1->Next;
    while (Tok2->Kind == TK_STR)
      Tok2 = Tok2->Next;

    int Len = Tok1->Ty->ArrayLen;
    for (Token *t = Tok1->Next; t != Tok2; t = t->Next)
      Len = Len + t->Ty->ArrayLen - 1;

    char *Buf = calloc(Tok1->Ty->Base->Size, Len);

    int I = 0;
    for (Token *t = Tok1; t != Tok2; t = t->Next) {
      memcpy(Buf + I, t->Str, t->Ty->Size);
      I = I + t->Ty->Size - t->Ty->Base->Size;
    }

    *Tok1 = *copyToken(Tok1);
    Tok1->Ty = arrayOf(Tok1->Ty->Base, Len);
    Tok1->Str = Buf;
    Tok1->Next = Tok2;
    Tok1 = Tok2;
  }
}

// 预处理器入口函数
Token *preprocess(Token *Tok) {
  initMacros();
  Tok = preprocess2(Tok);
  if (CondInclude)
    errorTok(CondInclude->Tok, "unterminated conditional directive");
  convertKeywords(Tok);
  joinAdjacentStringLiterals(Tok);
  return Tok;
}
