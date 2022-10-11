#include "rvcc.h"

typedef struct Macro Macro;
struct Macro {
  Macro *Next;
  char *Name;
  Token *Body;
  bool Deleted;
};

// `#if` can be nested, so we use a stack to manage nested `#if`s.
typedef struct CondIncl CondIncl;
struct CondIncl {
  CondIncl *Next;
  enum { IN_THEN, IN_ELIF, IN_ELSE } Ctx;
  Token *Tok;
  bool Included;
};

static Macro *Macros;
static CondIncl *CondInclude;

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
    if (isHash(Tok) && equal(Tok->Next, "if")) {
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
    if (isHash(Tok) && equal(Tok->Next, "if")) {
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

// Read and evaluate a constant expression.
static long evalConstExpr(Token **Rest, Token *Tok) {
  Token *Start = Tok;
  Token *Expr = copyLine(Rest, Tok->Next);

  if (Expr->Kind == TK_EOF)
    errorTok(Start, "no expression");

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

static Macro *addMacro(char *Name, Token *Body) {
  Macro *M = calloc(1, sizeof(Macro));
  M->Next = Macros;
  M->Name = Name;
  M->Body = Body;
  Macros = M;
  return M;
}

// If tok is a macro, expand it and return true.
// Otherwise, do nothing and return false.
static bool expandMacro(Token **Rest, Token *Tok) {
  Macro *M = findMacro(Tok);
  if (!M)
    return false;
  *Rest = append(M->Body, Tok->Next);
  return true;
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
      Tok = Tok->Next;

      if (Tok->Kind != TK_STR)
        errorTok(Tok, "expected a filename");

      char *Path;
      if (Tok->Str[0] == '/')
        Path = Tok->Str;
      else
        Path = format("%s/%s", dirname(strdup(Tok->File->Name)), Tok->Str);

      Token *Tok2 = tokenizeFile(Path);
      if (!Tok2)
        errorTok(Tok, "%s", strerror(errno));
      Tok = skipLine(Tok->Next);
      Tok = append(Tok2, Tok);
      continue;
    }

    if (equal(Tok, "define")) {
      Tok = Tok->Next;
      if (Tok->Kind != TK_IDENT)
        errorTok(Tok, "macro name must be an identifier");
      char *Name = strndup(Tok->Loc, Tok->Len);
      addMacro(Name, copyLine(&Tok, Tok->Next));
      continue;
    }

    if (equal(Tok, "undef")) {
      Tok = Tok->Next;
      if (Tok->Kind != TK_IDENT)
        errorTok(Tok, "macro name must be an identifier");
      char *Name = strndup(Tok->Loc, Tok->Len);
      Tok = skipLine(Tok->Next);

      Macro *M = addMacro(Name, NULL);
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

    // `#`-only line is legal. It's called a null directive.
    if (Tok->AtBOL)
      continue;

    errorTok(Tok, "invalid preprocessor directive");
  }

  Cur->Next = Tok;
  return Head.Next;
}

// 预处理器入口函数
Token *preprocess(Token *Tok) {
  Tok = preprocess2(Tok);
  if (CondInclude)
    errorTok(CondInclude->Tok, "unterminated conditional directive");
  convertKeywords(Tok);
  return Tok;
}
