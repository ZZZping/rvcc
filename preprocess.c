#include "rvcc.h"

// 预处理器入口函数
Token *preprocess(Token *Tok) {
  convertKeywords(Tok);
  return Tok;
}
