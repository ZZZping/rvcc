#include "rvcc.h"

static bool isHash(Token *Tok) { return Tok->AtBOL && equal(Tok, "#"); }

static Token *copyToken(Token *Tok) {
  Token *T = calloc(1, sizeof(Token));
  *T = *Tok;
  T->Next = NULL;
  return T;
}

// Append tok2 to the end of tok1.
static Token *append(Token *Tok1, Token *Tok2) {
  if (!Tok1 || Tok1->Kind == TK_EOF)
    return Tok2;

  Token Head = {};
  Token *Cur = &Head;

  for (; Tok1 && Tok1->Kind != TK_EOF; Tok1 = Tok1->Next)
    Cur = Cur->Next = copyToken(Tok1);
  Cur->Next = Tok2;
  return Head.Next;
}

// Visit all tokens in `tok` while evaluating preprocessing
// macros and directives.
static Token *preprocess2(Token *Tok) {
  Token Head = {};
  Token *Cur = &Head;

  while (Tok->Kind != TK_EOF) {
    // Pass through if it is not a "#".
    if (!isHash(Tok)) {
      Cur = Cur->Next = Tok;
      Tok = Tok->Next;
      continue;
    }

    Tok = Tok->Next;

    if (equal(Tok, "include")) {
      Tok = Tok->Next;

      if (Tok->Kind != TK_STR)
        errorTok(Tok, "expected a filename");

      char *Path = format("%s/%s", dirname(strdup(Tok->File->Name)), Tok->Str);
      Token *Tok2 = tokenizeFile(Path);
      if (!Tok2)
        errorTok(Tok, "%s", strerror(errno));
      Tok = append(Tok2, Tok->Next);
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
  convertKeywords(Tok);
  return Tok;
}
