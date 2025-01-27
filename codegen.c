#include "rvcc.h"

#define GP_MAX 8
#define FP_MAX 8

// 输出文件
static FILE *OutputFile;
// 记录栈深度
static int Depth;
// 当前的函数
static Obj *CurrentFn;

static void genExpr(Node *Nd);
static void genStmt(Node *Nd);

// 输出字符串到目标文件并换行
static void printLn(char *Fmt, ...) {
  va_list VA;

  va_start(VA, Fmt);
  vfprintf(OutputFile, Fmt, VA);
  va_end(VA);

  fprintf(OutputFile, "\n");
}

// 代码段计数
static int count(void) {
  static int I = 1;
  return I++;
}

// 压栈，将结果临时压入栈中备用
// sp为栈指针，栈反向向下增长，64位下，8个字节为一个单位，所以sp-8
// 当前栈指针的地址就是sp，将a0的值压入栈
// 不使用寄存器存储的原因是因为需要存储的值的数量是变化的。
static void push(void) {
  printLn("  # 压栈，将a0的值存入栈顶");
  printLn("  addi sp, sp, -8");
  printLn("  sd a0, 0(sp)");
  Depth++;
}

// 弹栈，将sp指向的地址的值，弹出到a1
static void pop(int Reg) {
  printLn("  # 弹栈，将栈顶的值存入a%d", Reg);
  printLn("  ld a%d, 0(sp)", Reg);
  printLn("  addi sp, sp, 8");
  Depth--;
}

// 对于浮点类型进行压栈
static void pushF(void) {
  printLn("  # 压栈，将fa0的值存入栈顶");
  printLn("  addi sp, sp, -8");
  printLn("  fsd fa0, 0(sp)");
  Depth++;
}

// 对于浮点类型进行弹栈
static void popF(int Reg) {
  printLn("  # 弹栈，将栈顶的值存入fa%d", Reg);
  printLn("  fld fa%d, 0(sp)", Reg);
  printLn("  addi sp, sp, 8");
  Depth--;
}

// 对齐到Align的整数倍
int alignTo(int N, int Align) {
  // (0,Align]返回Align
  return (N + Align - 1) / Align * Align;
}

// 计算给定节点的绝对地址
// 如果报错，说明节点不在内存中
static void genAddr(Node *Nd) {
  switch (Nd->Kind) {
  // 变量
  case ND_VAR:
    if (Nd->Var->IsLocal) { // 偏移量是相对于fp的
      printLn("  # 获取局部变量%s的栈内地址为%d(fp)", Nd->Var->Name,
              Nd->Var->Offset);
      printLn("  li t0, %d", Nd->Var->Offset);
      printLn("  add a0, fp, t0");
    } else {
      // 函数或者全局变量
      printLn("  # 获取%s%s的地址",
              Nd->Ty->Kind == TY_FUNC ? "函数" : "全局变量", Nd->Var->Name);
      printLn("  la a0, %s", Nd->Var->Name);
    }
    return;
  // 解引用*
  case ND_DEREF:
    genExpr(Nd->LHS);
    return;
  // 逗号
  case ND_COMMA:
    genExpr(Nd->LHS);
    genAddr(Nd->RHS);
    return;
  // 结构体成员
  case ND_MEMBER:
    genAddr(Nd->LHS);
    printLn("  # 计算成员变量的地址偏移量");
    printLn("  addi a0, a0, %d", Nd->Mem->Offset);
    return;
  // 函数调用
  case ND_FUNCALL:
    if (Nd->RetBuffer) {
      genExpr(Nd);
      return;
    }
    break;
  default:
    break;
  }

  errorTok(Nd->Tok, "not an lvalue");
}

// 加载a0指向的值
static void load(Type *Ty) {
  switch (Ty->Kind) {
  case TY_ARRAY:
  case TY_STRUCT:
  case TY_UNION:
  case TY_FUNC:
    return;
  case TY_FLOAT:
    printLn("  # 访问a0中存放的地址，取得的值存入fa0");
    printLn("  flw fa0, 0(a0)");
    return;
  case TY_DOUBLE:
    printLn("  # 访问a0中存放的地址，取得的值存入fa0");
    printLn("  fld fa0, 0(a0)");
    return;
  default:
    break;
  }

  // 添加无符号类型的后缀u
  char *Suffix = Ty->IsUnsigned ? "u" : "";

  printLn("  # 读取a0中存放的地址，得到的值存入a0");
  if (Ty->Size == 1)
    printLn("  lb%s a0, 0(a0)", Suffix);
  else if (Ty->Size == 2)
    printLn("  lh%s a0, 0(a0)", Suffix);
  else if (Ty->Size == 4)
    printLn("  lw%s a0, 0(a0)", Suffix);
  else
    printLn("  ld a0, 0(a0)");
}

// 将栈顶值(为一个地址)存入a0
static void store(Type *Ty) {
  pop(1);

  switch (Ty->Kind) {
  case TY_STRUCT:
  case TY_UNION:
    printLn("  # 对%s进行赋值", Ty->Kind == TY_STRUCT ? "结构体" : "联合体");
    for (int I = 0; I < Ty->Size; ++I) {
      printLn("  lb t0, %d(a0)", I);
      printLn("  sb t0, %d(a1)", I);
    }
    return;
  case TY_FLOAT:
    printLn("  # 将fa0的值，写入到a1中存放的地址");
    printLn("  fsw fa0, 0(a1)");
    return;
  case TY_DOUBLE:
    printLn("  # 将fa0的值，写入到a1中存放的地址");
    printLn("  fsd fa0, 0(a1)");
    return;
  default:
    break;
  }

  printLn("  # 将a0的值，写入到a1中存放的地址");
  if (Ty->Size == 1)
    printLn("  sb a0, 0(a1)");
  else if (Ty->Size == 2)
    printLn("  sh a0, 0(a1)");
  else if (Ty->Size == 4)
    printLn("  sw a0, 0(a1)");
  else
    printLn("  sd a0, 0(a1)");
};

// 用于结构体的成员变量的传参
void loadReg(char *Reg, int Offset, Type *Ty) {
  printLn("  li %s, 0", Reg);

  // 添加无符号类型的后缀u
  char *Suffix = Ty->IsUnsigned ? "u" : "";

  printLn("  # 读取%d(a0)中存放的地址，得到的值存入%s", Offset, Reg);
  if (Ty->Size == 1)
    printLn("  lb%s %s, %d(a0)", Suffix, Reg, Offset);
  else if (Ty->Size == 2)
    printLn("  lh%s %s, %d(a0)", Suffix, Reg, Offset);
  else if (Ty->Size == 4)
    printLn("  lw%s %s, %d(a0)", Suffix, Reg, Offset);
  else
    printLn("  ld %s, %d(a0)", Reg, Offset);
}

// 与0进行比较，不等于0则置1
static void notZero(Type *Ty) {
  switch (Ty->Kind) {
  case TY_FLOAT:
    printLn("  # 判断fa1是否不为0，为0置0，非0置1");
    printLn("  fmv.s.x fa1, zero");
    printLn("  feq.s a0, fa0, fa1");
    printLn("  xori a0, a0, 1");
    return;
  case TY_DOUBLE:
    printLn("  # 判断fa1是否不为0，为0置0，非0置1");
    printLn("  fmv.d.x fa1, zero");
    printLn("  feq.d a0, fa0, fa1");
    printLn("  xori a0, a0, 1");
    return;
  default:
    return;
  }
}

// 类型枚举
enum { I8, I16, I32, I64, U8, U16, U32, U64, F32, F64 };

// 获取类型对应的枚举值
static int getTypeId(Type *Ty) {
  switch (Ty->Kind) {
  case TY_CHAR:
    return Ty->IsUnsigned ? U8 : I8;
  case TY_SHORT:
    return Ty->IsUnsigned ? U16 : I16;
  case TY_INT:
    return Ty->IsUnsigned ? U32 : I32;
  case TY_LONG:
    return Ty->IsUnsigned ? U64 : I64;
  case TY_FLOAT:
    return F32;
  case TY_DOUBLE:
    return F64;
  default:
    return U64;
  }
}

// 类型映射表
// 先逻辑左移N位，再算术右移N位，就实现了将64位有符号数转换为64-N位的有符号数
static char i64i8[] = "  # 转换为i8类型\n"
                      "  slli a0, a0, 56\n"
                      "  srai a0, a0, 56";
static char i64i16[] = "  # 转换为i16类型\n"
                       "  slli a0, a0, 48\n"
                       "  srai a0, a0, 48";
static char i64i32[] = "  # 转换为i32类型\n"
                       "  slli a0, a0, 32\n"
                       "  srai a0, a0, 32";

// 先逻辑左移N位，再逻辑右移N位，就实现了将64位无符号数转换为64-N位的无符号数
static char i64u8[] = "  # 转换为u8类型\n"
                      "  slli a0, a0, 56\n"
                      "  srli a0, a0, 56";
static char i64u16[] = "  # 转换为u16类型\n"
                       "  slli a0, a0, 48\n"
                       "  srli a0, a0, 48";
static char i64u32[] = "  # 转换为u32类型\n"
                       "  slli a0, a0, 32\n"
                       "  srli a0, a0, 32";

// 有符号整型转换为浮点数
static char i64f32[] = "  # i64转换为f32类型\n"
                       "  fcvt.s.l fa0, a0";
static char i64f64[] = "  # i64转换为f64类型\n"
                       "  fcvt.d.l fa0, a0";

// 无符号整型转换为浮点数
static char u64f32[] = "  # u64转换为f32类型\n"
                       "  fcvt.s.lu fa0, a0";
static char u64f64[] = "  # u64转换为f64类型\n"
                       "  fcvt.d.lu fa0, a0";

// 单精度浮点数转换为整型
static char f32i8[] = "  # f32转换为i8类型\n"
                      "  fcvt.w.s a0, fa0, rtz\n"
                      "  slli a0, a0, 56\n"
                      "  srai a0, a0, 56\n";
static char f32i16[] = "  # f32转换为i16类型\n"
                       "  fcvt.w.s a0, fa0, rtz\n"
                       "  slli a0, a0, 48\n"
                       "  srai a0, a0, 48\n";
static char f32i32[] = "  # f32转换为i32类型\n"
                       "  fcvt.w.s a0, fa0, rtz";
static char f32i64[] = "  # f32转换为i64类型\n"
                       "  fcvt.l.s a0, fa0, rtz";

// 无符号整型转换为无符号浮点数
static char f32u8[] = "  # f32转换为u8类型\n"
                      "  fcvt.wu.s a0, fa0, rtz\n"
                      "  slli a0, a0, 56\n"
                      "  srli a0, a0, 56\n";
static char f32u16[] = "  # f32转换为u16类型\n"
                       "  fcvt.wu.s a0, fa0, rtz\n"
                       "  slli a0, a0, 48\n"
                       "  srli a0, a0, 48\n";
static char f32u32[] = "  # f32转换为u32类型\n"
                       "  fcvt.wu.s a0, fa0, rtz";
static char f32u64[] = "  # f32转换为u64类型\n"
                       "  fcvt.lu.s a0, fa0, rtz";

// 单精度转换为双精度浮点数
static char f32f64[] = "  # f32转换为f64类型\n"
                       "  fcvt.d.s fa0, fa0";

// 双精度浮点数转换为整型
static char f64i8[] = "  # f64转换为i8类型\n"
                      "  fcvt.w.d a0, fa0, rtz\n"
                      "  slli a0, a0, 56\n"
                      "  srai a0, a0, 56\n";
static char f64i16[] = "  # f64转换为i16类型\n"
                       "  fcvt.w.d a0, fa0, rtz\n"
                       "  slli a0, a0, 48\n"
                       "  srai a0, a0, 48\n";
static char f64i32[] = "  # f64转换为i32类型\n"
                       "  fcvt.w.d a0, fa0, rtz";
static char f64i64[] = "  # f64转换为i64类型\n"
                       "  fcvt.l.d a0, fa0, rtz";

// 双精度浮点数转换为无符号整型
static char f64u8[] = "  # f64转换为u8类型\n"
                      "  fcvt.wu.d a0, fa0, rtz\n"
                      "  slli a0, a0, 56\n"
                      "  srli a0, a0, 56\n";
static char f64u16[] = "  # f64转换为u16类型\n"
                       "  fcvt.wu.d a0, fa0, rtz\n"
                       "  slli a0, a0, 48\n"
                       "  srli a0, a0, 48\n";
static char f64u32[] = "  # f64转换为u32类型\n"
                       "  fcvt.wu.d a0, fa0, rtz";
static char f64u64[] = "  # f64转换为u64类型\n"
                       "  fcvt.lu.d a0, fa0, rtz";

// 双精度转换为单精度浮点数
static char f64f32[] = "  # f64转换为f32类型\n"
                       "  fcvt.s.d fa0, fa0";

// 所有类型转换表
static char *castTable[10][10] = {
    // clang-format off

    // 被映射到
    // {i8,  i16,    i32,    i64,    u8,    u16,    u32,    u64,    f32,    f64}
    {NULL,   NULL,   NULL,   NULL,   NULL,  NULL,   NULL,   NULL,   i64f32, i64f64}, // 从i8转换
    {i64i8,  NULL,   NULL,   NULL,   i64u8, NULL,   NULL,   NULL,   i64f32, i64f64}, // 从i16转换
    {i64i8,  i64i16, NULL,   NULL,   i64u8, i64u16, NULL,   NULL,   i64f32, i64f64}, // 从i32转换
    {i64i8,  i64i16, i64i32, NULL,   i64u8, i64u16, i64u32, NULL,   i64f32, i64f64}, // 从i64转换

    {i64i8,  NULL,   NULL,   NULL,   NULL,  NULL,   NULL,   NULL,   u64f32, u64f64}, // 从u8转换
    {i64i8,  i64i16, NULL,   NULL,   i64u8, NULL,   NULL,   NULL,   u64f32, u64f64}, // 从u16转换
    {i64i8,  i64i16, i64i32, NULL,   i64u8, i64u16, NULL,   NULL,   u64f32, u64f64}, // 从u32转换
    {i64i8,  i64i16, i64i32, NULL,   i64u8, i64u16, i64u32, NULL,   u64f32, u64f64}, // 从u64转换

    {f32i8,  f32i16, f32i32, f32i64, f32u8, f32u16, f32u32, f32u64, NULL,   f32f64}, // 从f32转换
    {f64i8,  f64i16, f64i32, f64i64, f64u8, f64u16, f64u32, f64u64, f64f32, NULL  }, // 从f64转换

    // clang-format on
};

// 类型转换
static void cast(Type *From, Type *To) {
  if (To->Kind == TY_VOID)
    return;

  if (To->Kind == TY_BOOL) {
    notZero(From);
    printLn("  # 转为bool类型：为0置0，非0置1");
    printLn("  snez a0, a0");
    return;
  }

  // 获取类型的枚举值
  int T1 = getTypeId(From);
  int T2 = getTypeId(To);
  if (castTable[T1][T2]) {
    printLn("  # 转换函数");
    printLn("%s", castTable[T1][T2]);
  }
}

// 解决带浮点的结构体（成员数<=2）
static void pushStructF(Type *Ty, int N) {
  if (N == 1) {
    // 只有一个浮点成员变量
    printLn("  # struct使用1个寄存器处理浮点");
    printLn("  addi sp, sp, -8");

    Member *Mem = Ty->Mems;
    loadReg("t0", Mem->Offset, Mem->Ty);

    printLn("  sd t0, 0(sp)");
    Depth += 1;
  } else {
    // 有一个或两个浮点成员变量
    printLn("  # struct使用2个寄存器处理浮点");
    printLn("  addi sp, sp, -16");

    // 第一个成员变量
    Member *Mem = Ty->Mems;
    loadReg("t0", Mem->Offset, Mem->Ty);
    // 第二个成员变量
    Mem = Mem->Next;
    loadReg("t1", Mem->Offset, Mem->Ty);

    printLn("  sd t1, 8(sp)");
    printLn("  sd t0, 0(sp)");
    Depth += 2;
  }
}

static void pushStruct(Type *Ty) {
  // 结构体小于16字节使用寄存器传递
  if (Ty->Size <= 16) {
    if (Ty->Size <= 8) {
      // 小于等于8字节，使用一个寄存器
      printLn("  # struct使用1个寄存器");
      printLn("  addi sp, sp, -8");
    } else {
      // 大于8字节，小于等于16字节，使用一个寄存器
      printLn("  # struct使用2个寄存器");
      printLn("  addi sp, sp, -16");
    }

    // 在寄存器内的偏移量
    int OffsetBitInReg = 0;
    // 寄存器内的最大的对齐量
    int MaxAlign = 8;
    // 当前存放值的寄存器
    char *Reg = "t0";
    printLn("  # t0寄存器清0，启用第一个寄存器");
    printLn("  li t0, 0");
    for (Member *Mem = Ty->Mems; Mem; Mem = Mem->Next) {
      unsigned MemBitSize = Mem->Ty->Size * 8;
      printLn("  # ***************%s*%d**************", Reg, MemBitSize);
      // 将偏移量对齐到类型的大小
      MaxAlign = (MaxAlign > MemBitSize) ? MaxAlign : MemBitSize;
      OffsetBitInReg = alignTo(OffsetBitInReg, MaxAlign);
      // 超过一个寄存器尺寸的时候，启用第二个寄存器
      if (OffsetBitInReg + MemBitSize > 64) {
        Reg = "t1";
        printLn("  # t1寄存器清0，启用第二个寄存器");
        printLn("  li t1, 0");
        OffsetBitInReg = 0;
        MaxAlign = 8;
      }

      // 将成员变量值加载到t2，然后左移到偏移的位置，然后加入a0中
      printLn("  # 将成员变量加载到寄存器中的正确位置");
      loadReg("t2", Mem->Offset, Mem->Ty);
      printLn("  slli t2, t2, %d", OffsetBitInReg);
      printLn("  add %s, %s, t2", Reg, Reg);

      // 为下一个成员变量计算偏移量
      OffsetBitInReg += MemBitSize;
    }

    if (Ty->Size <= 8) {
      printLn("  # 压栈一个寄存器来传递struct");
      printLn("  sd t0, 0(sp)");
      Depth += 1;
    } else {
      printLn("  # 压栈两个寄存器来传递struct");
      printLn("  sd t1, 8(sp)");
      printLn("  sd t0, 0(sp)");
      Depth += 2;
    }
  } else {
    printLn("  # 使用地址传递大于16字节的结构体");
    printLn("  addi sp, sp, -8");
    printLn("  sd a0, 0(sp)");
    Depth += 1;
  }
}

// 计算表达式后压栈
static void pushArgs2(Node *Args) {
  if (!Args)
    return;

  // 遍历到底部
  pushArgs2(Args->Next);

  printLn("\n  # ↓对表达式进行计算，然后压栈↓");
  genExpr(Args);

  switch (Args->Ty->Kind) {
  case TY_STRUCT:
  case TY_UNION: {
    // 计算结构体成员变量数
    int N = 0;
    // 判断是否成员变量为浮点数
    bool HasFloat = false;
    for (Member *Mem = Args->Ty->Mems; Mem; Mem = Mem->Next) {
      N++;
      if (isFloNum(Mem->Ty))
        HasFloat = true;
    }

    if (N <= 2 && HasFloat)
      // 两个以内带浮点结构体特殊处理
      pushStructF(Args->Ty, N);
    else
      // 其他情况统一处理
      pushStruct(Args->Ty);
    break;
  }
  case TY_FLOAT:
  case TY_DOUBLE:
    printLn("  # 对浮点参数表达式进行计算后压栈");
    pushF();
    break;
  default:
    printLn("  # 对整型参数表达式进行计算后压栈");
    push();
  }
  printLn("  # ↑结束压栈↑");
}

// 处理参数
static int pushArgs(Node *Nd) {
  int Stack = 0, GP = 0, FP = 0;

  // If the return type is a large struct/union, the caller passes
  // a pointer to a buffer as if it were the first argument.
  if (Nd->RetBuffer && Nd->Ty->Size > 16)
    GP++;

  // Load as many arguments to the registers as possible.
  for (Node *Arg = Nd->Args; Arg; Arg = Arg->Next) {
    Type *Ty = Arg->Ty;

    switch (Ty->Kind) {
    case TY_STRUCT:
    case TY_UNION:
      // TODO：解决寄存器传递struct时候GP、FP不够用的情况
      // TODO：解决两个寄存器传递时，只剩一个GP可用时，后一半用栈传递的情况
      break;
    case TY_FLOAT:
    case TY_DOUBLE:
      // 浮点优先使用FP，而后是GP，最后是栈传递
      if (FP >= FP_MAX && GP >= GP_MAX) {
        printLn("  # %f值通过栈传递", Arg->FVal);
        Arg->PassByStack = true;
        Stack++;
      } else if (FP < FP_MAX) {
        printLn("  # %f值通过fa%d传递", Arg->FVal, FP);
        FP++;
      } else {
        printLn("  # %f值通过a%d传递", Arg->FVal, GP);
        GP++;
      }
      break;
    default:
      // 整型优先使用GP，最后是栈传递
      if (GP >= GP_MAX) {
        printLn("  # %d值通过栈传递", Arg->Val);
        Arg->PassByStack = true;
        Stack++;
      } else {
        printLn("  # %d值通过a%d传递", Arg->Val, GP);
        GP++;
      }
    }
  }

  if ((Depth + Stack) % 2 == 1) {
    printLn("  # 对齐栈边界到16字节");
    printLn("  addi sp, sp, -8");
    Depth++;
    Stack++;
  }

  // 第一遍处理栈传递的参数
  pushArgs2(Nd->Args);

  // If the return type is a large struct/union, the caller passes
  // a pointer to a buffer as if it were the first argument.
  if (Nd->RetBuffer && Nd->Ty->Size > 16) {
    printLn("  li t0, %d", Nd->RetBuffer->Offset);
    printLn("  add a0, fp, t0");
    push();
  }

  return Stack;
}

static void copyRetBuffer(Obj *Var) {
  Type *Ty = Var->Ty;
  int GP = 0, FP = 0;

  int N = 0;
  bool HasFloat = false;
  for (Member *Mem = Ty->Mems; Mem; Mem = Mem->Next) {
    N++;
    if (isFloNum(Mem->Ty))
      HasFloat = true;
  }

  printLn("  # 复制返回值缓冲区：加载struct地址到t0");
  printLn("  li t0, %d", Var->Offset);
  printLn("  add t0, fp, t0");

  Member *Mem1 = Ty->Mems;
  if (N == 1) {
    if (isFloNum(Mem1->Ty))
      printLn("  fsd fa%d, 0(t0)", FP++);
    else
      printLn("  sd a%d, 0(t0)", GP++);

    return;
  }

  Member *Mem2 = Mem1->Next;
  // 第一个若是float，第二个偏移量为4，否则为8
  int Offset = 8;
  if (N == 2 && HasFloat) {
    if (isFloNum(Mem1->Ty)) {
      if (Mem1->Ty->Size == 4) {
        printLn("  fsw fa%d, 0(t0)", FP++);
        Offset = 4;
      } else {
        printLn("  fsd fa%d, 0(t0)", FP++);
      }
    } else {
      switch (Mem1->Ty->Size) {
      case 1:
        printLn("  sb a%d, 0(t0)", GP++);
        break;
      case 2:
        printLn("  sh a%d, 0(t0)", GP++);
        break;
      case 4:
        printLn("  sw a%d, 0(t0)", GP++);
        break;
      default:
        printLn("  sd a%d, 0(t0)", GP++);
        break;
      }
    }

    if (isFloNum(Mem2->Ty)) {
      if (Mem2->Ty->Size == 4)
        printLn("  fsw fa%d, %d(t0)", FP++, Offset);
      else
        printLn("  fsd fa%d, %d(t0)", FP++, Offset);
    } else {
      switch (Mem2->Ty->Size) {
      case 1:
        printLn("  sb a%d, %d(t0)", GP++, Offset);
        break;
      case 2:
        printLn("  sh a%d, %d(t0)", GP++, Offset);
        break;
      case 4:
        printLn("  sw a%d, %d(t0)", GP++, Offset);
        break;
      default:
        printLn("  sd a%d, %d(t0)", GP++, Offset);
        break;
      }
    }

    return;
  }

  printLn("  sd a%d, 0(t0)", GP++);
  printLn("  sd a%d, %d(t0)", GP++, 8);
}

static void copyStructReg(void) {
  Type *Ty = CurrentFn->Ty->ReturnTy;
  int GP = 0, FP = 0;

  printLn("  # 复制结构体寄存器：将存有struct地址的a0存入t0");
  printLn("  mv t0, a0");

  int N = 0;
  bool HasFloat = false;
  for (Member *Mem = Ty->Mems; Mem; Mem = Mem->Next) {
    N++;
    if (isFloNum(Mem->Ty))
      HasFloat = true;
  }

  Member *Mem1 = Ty->Mems;
  if (N == 1) {
    if (isFloNum(Mem1->Ty)) {
      if (Mem1->Ty->Size == 4)
        printLn("  flw fa%d, 0(t0)", FP++);
      else
        printLn("  fld fa%d, 0(t0)", FP++);
    } else {
      switch (Mem1->Ty->Size) {
      case 1:
        printLn("  lb a%d, 0(t0)", GP++);
        break;
      case 2:
        printLn("  lh a%d, 0(t0)", GP++);
        break;
      case 4:
        printLn("  lw a%d, 0(t0)", GP++);
        break;
      default:
        printLn("  ld a%d, 0(t0)", GP++);
        break;
      }
    }

    return;
  }

  Member *Mem2 = Mem1->Next;
  int Offset = 8;
  if (N == 2 && HasFloat) {
    if (isFloNum(Mem1->Ty)) {
      if (Mem1->Ty->Size == 4) {
        printLn("  flw fa%d, 0(t0)", FP++);
        Offset = 4;
      } else {
        printLn("  fld fa%d, 0(t0)", FP++);
      }
    } else {
      switch (Mem1->Ty->Size) {
      case 1:
        printLn("  lb a%d, 0(t0)", GP++);
        break;
      case 2:
        printLn("  lh a%d, 0(t0)", GP++);
        break;
      case 4:
        printLn("  lw a%d, 0(t0)", GP++);
        break;
      default:
        printLn("  ld a%d, 0(t0)", GP++);
        break;
      }
    }

    if (isFloNum(Mem2->Ty)) {
      if (Mem2->Ty->Size == 4) {
        printLn("  flw fa%d, %d(t0)", FP++, Offset);
      } else {
        printLn("  fld fa%d, %d(t0)", FP++, Offset);
      }
    } else {
      switch (Mem2->Ty->Size) {
      case 1:
        printLn("  lb a%d, %d(t0)", GP++, Offset);
        break;
      case 2:
        printLn("  lh a%d, %d(t0)", GP++, Offset);
        break;
      case 4:
        printLn("  lw a%d, %d(t0)", GP++, Offset);
        break;
      default:
        printLn("  ld a%d, %d(t0)", GP++, Offset);
        break;
      }
    }

    return;
  }

  printLn("  ld a%d, 0(t0)", GP++);
  printLn("  ld a%d, %d(t0)", GP++, Offset);
}

static void copyStructMem(void) {
  // Type *Ty = CurrentFn->Ty->ReturnTy;
  // Obj *var = CurrentFn->Params;

  // printLn("  # 复制结构体内存：将struct地址存入t0");
  // printLn("  li t0, %d", var->Offset);
  // printLn("  add t0, fp, t0", var->Offset);

  // printLn("  # 复制结构体内存：复制所有字节");
  // for (int i = 0; i < Ty->Size; i++) {
  //   printLn("  lb t1, %d(t0)", i);
  //   printLn("  sb t1, %d(t0)", i);
  // }
}

// 生成表达式
static void genExpr(Node *Nd) {
  // .loc 文件编号 行号
  printLn("  .loc %d %d", Nd->Tok->File->FileNo, Nd->Tok->LineNo);

  // 生成各个根节点
  switch (Nd->Kind) {
  // 空表达式
  case ND_NULL_EXPR:
    return;
  // 加载数字到a0

  // float和uint32、double和uint64 共用一份内存空间
  case ND_NUM: {
    union {
      float F32;
      double F64;
      uint32_t U32;
      uint64_t U64;
    } U;

    switch (Nd->Ty->Kind) {
    case TY_FLOAT:
      U.F32 = Nd->FVal;
      printLn("  # 将%f加载到fa0中", Nd->FVal);
      printLn("  li a0, %u  # float %f", U.U32, Nd->FVal);
      printLn("  fmv.w.x fa0, a0");
      return;
    case TY_DOUBLE:
      printLn("  # 将%f加载到fa0中", Nd->FVal);
      U.F64 = Nd->FVal;
      printLn("  li a0, %lu  # double %f", U.U64, Nd->FVal);
      printLn("  fmv.d.x fa0, a0");
      return;
    default:
      printLn("  # 将%d加载到a0中", Nd->Val);
      printLn("  li a0, %ld", Nd->Val);
      return;
    }
  }
  // 对寄存器取反
  case ND_NEG:
    // 计算左部的表达式
    genExpr(Nd->LHS);

    switch (Nd->Ty->Kind) {
    case TY_FLOAT:
      printLn("  # 对float类型的fa0值进行取反");
      printLn("  fneg.s fa0, fa0");
      return;
    case TY_DOUBLE:
      printLn("  # 对double类型的fa0值进行取反");
      printLn("  fneg.d fa0, fa0");
      return;
    default:
      // neg a0, a0是sub a0, x0, a0的别名, 即a0=0-a0
      printLn("  # 对a0值进行取反");
      printLn("  neg a0, a0");
      return;
    }
  // 变量
  case ND_VAR:
  case ND_MEMBER:
    // 计算出变量的地址，然后存入a0
    genAddr(Nd);
    load(Nd->Ty);
    return;
  // 解引用
  case ND_DEREF:
    genExpr(Nd->LHS);
    load(Nd->Ty);
    return;
  // 取地址
  case ND_ADDR:
    genAddr(Nd->LHS);
    return;
  // 赋值
  case ND_ASSIGN:
    // 左部是左值，保存值到的地址
    genAddr(Nd->LHS);
    push();
    // 右部是右值，为表达式的值
    genExpr(Nd->RHS);
    store(Nd->Ty);
    return;
  // 语句表达式
  case ND_STMT_EXPR:
    for (Node *N = Nd->Body; N; N = N->Next)
      genStmt(N);
    return;
  // 逗号
  case ND_COMMA:
    genExpr(Nd->LHS);
    genExpr(Nd->RHS);
    return;
  // 类型转换
  case ND_CAST:
    genExpr(Nd->LHS);
    cast(Nd->LHS->Ty, Nd->Ty);
    return;
  // 内存清零
  case ND_MEMZERO: {
    printLn("  # 对%s的内存%d(fp)清零%d位", Nd->Var->Name, Nd->Var->Offset,
            Nd->Var->Ty->Size);
    // 对栈内变量所占用的每个字节都进行清零
    for (int I = 0; I < Nd->Var->Ty->Size; I++) {
      printLn("  li t0, %d", Nd->Var->Offset + I);
      printLn("  add t0, fp, t0");
      printLn("  sb zero, 0(t0)");
    }
    return;
  }
  // 条件运算符
  case ND_COND: {
    int C = count();
    printLn("\n# =====条件运算符%d===========", C);
    genExpr(Nd->Cond);
    notZero(Nd->Cond->Ty);
    printLn("  # 条件判断，为0则跳转");
    printLn("  beqz a0, .L.else.%d", C);
    genExpr(Nd->Then);
    printLn("  # 跳转到条件运算符结尾部分");
    printLn("  j .L.end.%d", C);
    printLn(".L.else.%d:", C);
    genExpr(Nd->Els);
    printLn(".L.end.%d:", C);
    return;
  }
  // 非运算
  case ND_NOT:
    genExpr(Nd->LHS);
    notZero(Nd->LHS->Ty);
    printLn("  # 非运算");
    // a0=0则置1，否则为0
    printLn("  seqz a0, a0");
    return;
  // 逻辑与
  case ND_LOGAND: {
    int C = count();
    printLn("\n# =====逻辑与%d===============", C);
    genExpr(Nd->LHS);
    // 判断是否为短路操作
    notZero(Nd->LHS->Ty);
    printLn("  # 左部短路操作判断，为0则跳转");
    printLn("  beqz a0, .L.false.%d", C);
    genExpr(Nd->RHS);
    notZero(Nd->RHS->Ty);
    printLn("  # 右部判断，为0则跳转");
    printLn("  beqz a0, .L.false.%d", C);
    printLn("  li a0, 1");
    printLn("  j .L.end.%d", C);
    printLn(".L.false.%d:", C);
    printLn("  li a0, 0");
    printLn(".L.end.%d:", C);
    return;
  }
  // 逻辑或
  case ND_LOGOR: {
    int C = count();
    printLn("\n# =====逻辑或%d===============", C);
    genExpr(Nd->LHS);
    notZero(Nd->LHS->Ty);
    // 判断是否为短路操作
    printLn("  # 左部短路操作判断，不为0则跳转");
    printLn("  bnez a0, .L.true.%d", C);
    genExpr(Nd->RHS);
    notZero(Nd->RHS->Ty);
    printLn("  # 右部判断，不为0则跳转");
    printLn("  bnez a0, .L.true.%d", C);
    printLn("  li a0, 0");
    printLn("  j .L.end.%d", C);
    printLn(".L.true.%d:", C);
    printLn("  li a0, 1");
    printLn(".L.end.%d:", C);
    return;
  }
  // 按位取非运算
  case ND_BITNOT:
    genExpr(Nd->LHS);
    printLn("  # 按位取反");
    // 这里的 not a0, a0 为 xori a0, a0, -1 的伪码
    printLn("  not a0, a0");
    return;
  // 函数调用
  case ND_FUNCALL: {
    // 计算所有参数的值，正向压栈
    // 此处获取到栈传递参数的数量
    int StackArgs = pushArgs(Nd);
    genExpr(Nd->LHS);
    // 将a0的值存入t0
    printLn("  mv t0, a0");

    // 反向弹栈，a0->参数1，a1->参数2……
    int GP = 0, FP = 0;

    // If the return type is a large struct/union, the caller passes
    // a pointer to a buffer as if it were the first argument.
    if (Nd->RetBuffer && Nd->Ty->Size > 16)
      pop(GP++);

    // 读取函数形参中的参数类型
    Type *CurArg = Nd->FuncType->Params;
    for (Node *Arg = Nd->Args; Arg; Arg = Arg->Next) {
      // 如果是可变参数函数
      // 匹配到空参数（最后一个）的时候，将剩余的整型寄存器弹栈
      if (Nd->FuncType->IsVariadic && CurArg == NULL) {
        if (GP < GP_MAX) {
          printLn("  # a%d传递可变实参", GP);
          pop(GP++);
        }
        continue;
      }

      CurArg = CurArg->Next;
      Type *Ty = Arg->Ty;

      switch (Ty->Kind) {
      case TY_STRUCT:
      case TY_UNION: {
        // 如果成员变量小于2个，并且包含浮点成员变量特殊处理
        int N = 0;
        bool HasFloat = false;
        for (Member *Mem = Ty->Mems; Mem; Mem = Mem->Next) {
          N++;
          if (isFloNum(Mem->Ty))
            HasFloat = true;
        }

        // 只有一个浮点成员变量
        if (N == 1 && HasFloat) {
          popF(FP++);
          break;
        }

        // 有一个或两个浮点成员变量
        if (N == 2 && HasFloat) {
          for (Member *Mem = Ty->Mems; Mem; Mem = Mem->Next) {
            if (Mem->Ty->Kind == TY_FLOAT) {
              // 解决struct传带float两参时，float值为负数的问题
              printLn("  # 弹栈，将栈顶的值存入fa%d", FP);
              printLn("  flw fa%d, 0(sp)", FP++);
              printLn("  addi sp, sp, 8");
              Depth--;
            } else if (Mem->Ty->Kind == TY_DOUBLE) {
              popF(FP++);
            } else {
              pop(GP++);
            }
          }
          break;
        }

        // 其他全部通过整型寄存器的情况
        if (Ty->Size <= 8) {
          // struct使用一个寄存器
          pop(GP++);
        } else if (Ty->Size <= 16) {
          // struct使用二个寄存器
          pop(GP++);
          pop(GP++);
        } else {
          // struct使用地址传递
          pop(GP++);
        }
        break;
      }
      case TY_FLOAT:
      case TY_DOUBLE:
        if (FP < FP_MAX) {
          printLn("  # fa%d传递浮点参数", FP);
          popF(FP++);
        } else if (GP < GP_MAX) {
          printLn("  # a%d传递浮点参数", GP);
          pop(GP++);
        }
        break;
      default:
        if (GP < GP_MAX) {
          printLn("  # a%d传递整型参数", GP);
          pop(GP++);
        }
      }
    }

    // 调用函数
    printLn("  # 调用函数");
    printLn("  jalr t0");

    // 为栈传递的变量开辟栈空间
    if (StackArgs) {
      Depth -= StackArgs;
      printLn("  # 为栈传递的%d个变量开辟栈空间", StackArgs);
      printLn("  addi sp, sp, %d", StackArgs * 8);
    }

    // 清除寄存器中高位无关的数据
    switch (Nd->Ty->Kind) {
    case TY_BOOL:
      printLn("  # 清除bool类型的高位");
      printLn("  slli a0, a0, 63");
      printLn("  srli a0, a0, 63");
      return;
    case TY_CHAR:
      printLn("  # 清除char类型的高位");
      if (Nd->Ty->IsUnsigned) {
        printLn("  slli a0, a0, 56");
        printLn("  srli a0, a0, 56");
      } else {
        printLn("  slli a0, a0, 56");
        printLn("  srai a0, a0, 56");
      }
      return;
    case TY_SHORT:
      printLn("  # 清除short类型的高位");
      if (Nd->Ty->IsUnsigned) {
        printLn("  slli a0, a0, 48");
        printLn("  srli a0, a0, 48");
      } else {
        printLn("  slli a0, a0, 48");
        printLn("  srai a0, a0, 48");
      }
      return;
    default:
      break;
    }

    // 如果返回的结构体小于16字节，直接使用两个寄存器返回
    if (Nd->RetBuffer && Nd->Ty->Size <= 16) {
      copyRetBuffer(Nd->RetBuffer);
      printLn("  li t0, %d", Nd->RetBuffer->Offset);
      printLn("  add a0, fp, t0");
    }

    return;
  }
  default:
    break;
  }

  // 处理浮点类型
  if (isFloNum(Nd->LHS->Ty)) {
    // 递归到最右节点
    genExpr(Nd->RHS);
    // 将结果压入栈
    pushF();
    // 递归到左节点
    genExpr(Nd->LHS);
    // 将结果弹栈到fa1
    popF(1);

    // 生成各个二叉树节点
    // float对应s(single)后缀，double对应d(double)后缀
    char *Suffix = (Nd->LHS->Ty->Kind == TY_FLOAT) ? "s" : "d";

    switch (Nd->Kind) {
    case ND_ADD:
      printLn("  # fa0+fa1，结果写入fa0");
      printLn("  fadd.%s fa0, fa0, fa1", Suffix);
      return;
    case ND_SUB:
      printLn("  # fa0-fa1，结果写入fa0");
      printLn("  fsub.%s fa0, fa0, fa1", Suffix);
      return;
    case ND_MUL:
      printLn("  # fa0×fa1，结果写入fa0");
      printLn("  fmul.%s fa0, fa0, fa1", Suffix);
      return;
    case ND_DIV:
      printLn("  # fa0÷fa1，结果写入fa0");
      printLn("  fdiv.%s fa0, fa0, fa1", Suffix);
      return;
    case ND_EQ:
      printLn("  # 判断是否fa0=fa1");
      printLn("  feq.%s a0, fa0, fa1", Suffix);
      return;
    case ND_NE:
      printLn("  # 判断是否fa0≠fa1");
      printLn("  feq.%s a0, fa0, fa1", Suffix);
      printLn("  seqz a0, a0");
      return;
    case ND_LT:
      printLn("  # 判断是否fa0<fa1");
      printLn("  flt.%s a0, fa0, fa1", Suffix);
      return;
    case ND_LE:
      printLn("  # 判断是否fa0≤fa1");
      printLn("  fle.%s a0, fa0, fa1", Suffix);
      return;
    default:
      errorTok(Nd->Tok, "invalid expression");
    }
  }

  // 递归到最右节点
  genExpr(Nd->RHS);
  // 将结果压入栈
  push();
  // 递归到左节点
  genExpr(Nd->LHS);
  // 将结果弹栈到a1
  pop(1);

  // 生成各个二叉树节点
  char *Suffix = Nd->LHS->Ty->Kind == TY_LONG || Nd->LHS->Ty->Base ? "" : "w";
  switch (Nd->Kind) {
  case ND_ADD: // + a0=a0+a1
    printLn("  # a0+a1，结果写入a0");
    printLn("  add%s a0, a0, a1", Suffix);
    return;
  case ND_SUB: // - a0=a0-a1
    printLn("  # a0-a1，结果写入a0");
    printLn("  sub%s a0, a0, a1", Suffix);
    return;
  case ND_MUL: // * a0=a0*a1
    printLn("  # a0×a1，结果写入a0");
    printLn("  mul%s a0, a0, a1", Suffix);
    return;
  case ND_DIV: // / a0=a0/a1
    printLn("  # a0÷a1，结果写入a0");
    if (Nd->Ty->IsUnsigned)
      printLn("  divu%s a0, a0, a1", Suffix);
    else
      printLn("  div%s a0, a0, a1", Suffix);
    return;
  case ND_MOD: // % a0=a0%a1
    printLn("  # a0%%a1，结果写入a0");
    if (Nd->Ty->IsUnsigned)
      printLn("  remu%s a0, a0, a1", Suffix);
    else
      printLn("  rem%s a0, a0, a1", Suffix);
    return;
  case ND_BITAND: // & a0=a0&a1
    printLn("  # a0&a1，结果写入a0");
    printLn("  and a0, a0, a1");
    return;
  case ND_BITOR: // | a0=a0|a1
    printLn("  # a0|a1，结果写入a0");
    printLn("  or a0, a0, a1");
    return;
  case ND_BITXOR: // ^ a0=a0^a1
    printLn("  # a0^a1，结果写入a0");
    printLn("  xor a0, a0, a1");
    return;
  case ND_EQ:
  case ND_NE:
    // a0=a0^a1，异或指令
    printLn("  # 判断是否a0%sa1", Nd->Kind == ND_EQ ? "=" : "≠");
    printLn("  xor a0, a0, a1");

    if (Nd->Kind == ND_EQ)
      // a0==a1
      // a0=a0^a1, sltiu a0, a0, 1
      // 等于0则置1
      printLn("  seqz a0, a0");
    else
      // a0!=a1
      // a0=a0^a1, sltu a0, x0, a0
      // 不等于0则置1
      printLn("  snez a0, a0");
    return;
  case ND_LT:
    printLn("  # 判断a0<a1");
    if (Nd->LHS->Ty->IsUnsigned)
      printLn("  sltu a0, a0, a1");
    else
      printLn("  slt a0, a0, a1");
    return;
  case ND_LE:
    // a0<=a1等价于
    // a0=a1<a0, a0=a0^1
    printLn("  # 判断是否a0≤a1");
    if (Nd->LHS->Ty->IsUnsigned)
      printLn("  sltu a0, a1, a0");
    else
      printLn("  slt a0, a1, a0");
    printLn("  xori a0, a0, 1");
    return;
  case ND_SHL:
    printLn("  # a0逻辑左移a1位");
    printLn("  sll%s a0, a0, a1", Suffix);
    return;
  case ND_SHR:
    printLn("  # a0算术右移a1位");
    if (Nd->Ty->IsUnsigned)
      printLn("  srl%s a0, a0, a1", Suffix);
    else
      printLn("  sra%s a0, a0, a1", Suffix);
    return;
  default:
    break;
  }

  errorTok(Nd->Tok, "invalid expression");
}

// 生成语句
static void genStmt(Node *Nd) {
  // .loc 文件编号 行号
  printLn("  .loc %d %d", Nd->Tok->File->FileNo, Nd->Tok->LineNo);

  switch (Nd->Kind) {
  // 生成if语句
  case ND_IF: {
    // 代码段计数
    int C = count();
    printLn("\n# =====分支语句%d==============", C);
    // 生成条件内语句
    printLn("\n# Cond表达式%d", C);
    genExpr(Nd->Cond);
    notZero(Nd->Cond->Ty);
    // 判断结果是否为0，为0则跳转到else标签
    printLn("  # 若a0为0，则跳转到分支%d的.L.else.%d段", C, C);
    printLn("  beqz a0, .L.else.%d", C);
    // 生成符合条件后的语句
    printLn("\n# Then语句%d", C);
    genStmt(Nd->Then);
    // 执行完后跳转到if语句后面的语句
    printLn("  # 跳转到分支%d的.L.end.%d段", C, C);
    printLn("  j .L.end.%d", C);
    // else代码块，else可能为空，故输出标签
    printLn("\n# Else语句%d", C);
    printLn("# 分支%d的.L.else.%d段标签", C, C);
    printLn(".L.else.%d:", C);
    // 生成不符合条件后的语句
    if (Nd->Els)
      genStmt(Nd->Els);
    // 结束if语句，继续执行后面的语句
    printLn("\n# 分支%d的.L.end.%d段标签", C, C);
    printLn(".L.end.%d:", C);
    return;
  }
  // 生成for或while循环语句
  case ND_FOR: {
    // 代码段计数
    int C = count();
    printLn("\n# =====循环语句%d===============", C);
    // 生成初始化语句
    if (Nd->Init) {
      printLn("\n# Init语句%d", C);
      genStmt(Nd->Init);
    }
    // 输出循环头部标签
    printLn("\n# 循环%d的.L.begin.%d段标签", C, C);
    printLn(".L.begin.%d:", C);
    // 处理循环条件语句
    printLn("# Cond表达式%d", C);
    if (Nd->Cond) {
      // 生成条件循环语句
      genExpr(Nd->Cond);
      notZero(Nd->Cond->Ty);
      // 判断结果是否为0，为0则跳转到结束部分
      printLn("  # 若a0为0，则跳转到循环%d的%s段", C, Nd->BrkLabel);
      printLn("  beqz a0, %s", Nd->BrkLabel);
    }
    // 生成循环体语句
    printLn("\n# Then语句%d", C);
    genStmt(Nd->Then);
    // continue标签语句
    printLn("%s:", Nd->ContLabel);
    // 处理循环递增语句
    if (Nd->Inc) {
      printLn("\n# Inc语句%d", C);
      // 生成循环递增语句
      genExpr(Nd->Inc);
    }
    // 跳转到循环头部
    printLn("  # 跳转到循环%d的.L.begin.%d段", C, C);
    printLn("  j .L.begin.%d", C);
    // 输出循环尾部标签
    printLn("\n# 循环%d的%s段标签", C, Nd->BrkLabel);
    printLn("%s:", Nd->BrkLabel);
    return;
  }
  // 生成do while语句
  case ND_DO: {
    int C = count();
    printLn("\n# =====do while语句%d============", C);
    printLn("\n# begin语句%d", C);
    printLn(".L.begin.%d:", C);

    printLn("\n# Then语句%d", C);
    genStmt(Nd->Then);

    printLn("\n# Cond语句%d", C);
    printLn("%s:", Nd->ContLabel);
    genExpr(Nd->Cond);

    notZero(Nd->Cond->Ty);
    printLn("  # 跳转到循环%d的.L.begin.%d段", C, C);
    printLn("  bnez a0, .L.begin.%d", C);

    printLn("\n# 循环%d的%s段标签", C, Nd->BrkLabel);
    printLn("%s:", Nd->BrkLabel);
    return;
  }
  case ND_SWITCH:
    printLn("\n# =====switch语句===============");
    genExpr(Nd->Cond);

    printLn("  # 遍历跳转到值等于a0的case标签");
    for (Node *N = Nd->CaseNext; N; N = N->CaseNext) {
      printLn("  li t0, %ld", N->Val);
      printLn("  beq a0, t0, %s", N->Label);
    }

    if (Nd->DefaultCase) {
      printLn("  # 跳转到default标签");
      printLn("  j %s", Nd->DefaultCase->Label);
    }

    printLn("  # 结束switch，跳转break标签");
    printLn("  j %s", Nd->BrkLabel);
    // 生成case标签的语句
    genStmt(Nd->Then);
    printLn("# switch的break标签，结束switch");
    printLn("%s:", Nd->BrkLabel);
    return;
  case ND_CASE:
    printLn("# case标签，值为%d", Nd->Val);
    printLn("%s:", Nd->Label);
    genStmt(Nd->LHS);
    return;
  // 生成代码块，遍历代码块的语句链表
  case ND_BLOCK:
    for (Node *N = Nd->Body; N; N = N->Next)
      genStmt(N);
    return;
  // goto语句
  case ND_GOTO:
    printLn("  j %s", Nd->UniqueLabel);
    return;
  // 标签语句
  case ND_LABEL:
    printLn("%s:", Nd->UniqueLabel);
    genStmt(Nd->LHS);
    return;
  // 生成return语句
  case ND_RETURN:
    printLn("# 返回语句");
    // 不为空返回语句时
    if (Nd->LHS) {
      genExpr(Nd->LHS);

      Type *Ty = Nd->LHS->Ty;
      if (Ty->Kind == TY_STRUCT || Ty->Kind == TY_UNION) {
        if (Ty->Size <= 16)
          copyStructReg();
        else
          copyStructMem();
      }
    }
    // 无条件跳转语句，跳转到.L.return段
    // j offset是 jal x0, offset的别名指令
    printLn("  # 跳转到.L.return.%s段", CurrentFn->Name);
    printLn("  j .L.return.%s", CurrentFn->Name);
    return;
  // 生成表达式语句
  case ND_EXPR_STMT:
    genExpr(Nd->LHS);
    return;
  default:
    break;
  }

  errorTok(Nd->Tok, "invalid statement");
}

// 根据变量的链表计算出偏移量
static void assignLVarOffsets(Obj *Prog) {
  // 为每个函数计算其变量所用的栈空间
  for (Obj *Fn = Prog; Fn; Fn = Fn->Next) {
    // 如果不是函数,则终止
    if (!Fn->IsFunction)
      continue;

    // 被调用函数将自己的ra、fp也压入栈了，
    // 所以fp+16才是上一级函数的sp顶
    // /--------------上一级函数----------------/ sp(old)
    // /                 ra                   / sp(old)-8
    // /              fp（上上一级）            / fp(old) = sp(old)-16
    // /             栈保存的N个变量            / fp(old)-N*8
    // /---------------本级函数----------------/ sp = fp(old)-N*8
    // /                 ra                   / sp-8
    // /                fp（上一级）            / fp = sp-16

    int ReOffset = 16;
    // 计算栈传递参数的偏移量
    int GP = 0, FP = 0;
    for (Obj *Var = Fn->Params; Var; Var = Var->Next) {
      if (isFloNum(Var->Ty)) {
        if (FP < FP_MAX) {
          FP++;
          continue;
        } else if (GP < GP_MAX) {
          GP++;
          continue;
        }
      } else {
        if (GP++ < GP_MAX)
          continue;
      }

      // 对齐变量
      ReOffset = alignTo(ReOffset, 8);
      // 为栈传递变量赋一个偏移量，或者说是反向栈地址
      Var->Offset = ReOffset;
      // 栈传递变量计算反向偏移量
      ReOffset += Var->Ty->Size;
      printLn(" #  栈传递变量%s偏移量%d", Var->Name, Var->Offset);
    }

    int Offset = 0;
    // 读取所有变量
    for (Obj *Var = Fn->Locals; Var; Var = Var->Next) {
      // 已经处理过的直接跳过
      if (Var->Offset) {
        continue;
      }

      // 每个变量分配空间
      Offset += Var->Ty->Size;
      // 对齐变量
      Offset = alignTo(Offset, Var->Align);
      // 为每个变量赋一个偏移量，或者说是栈中地址
      Var->Offset = -Offset;
      printLn(" #  寄存器传递%s变量偏移量%d", Var->Name, Var->Offset);
    }
    // 将栈对齐到16字节
    Fn->StackSize = alignTo(Offset, 16);
  }
}

static void emitData(Obj *Prog) {
  for (Obj *Var = Prog; Var; Var = Var->Next) {
    // 跳过是函数或者无定义的变量
    if (Var->IsFunction || !Var->IsDefinition)
      continue;

    if (Var->IsStatic) {
      printLn("\n  # static全局变量%s", Var->Name);
      printLn("  .local %s", Var->Name);
    } else {
      printLn("\n  # 全局变量%s", Var->Name);
      printLn("  .globl %s", Var->Name);
    }

    printLn("  # 对齐全局变量");
    if (!Var->Ty->Align)
      error("Align can not be 0!");
    printLn("  .align %d", (int)log2(Var->Align));
    // 判断是否有初始值
    if (Var->InitData) {
      printLn("\n  # 数据段标签");
      printLn("  .data");
      printLn("%s:", Var->Name);
      Relocation *Rel = Var->Rel;
      int Pos = 0;
      while (Pos < Var->Ty->Size) {
        if (Rel && Rel->Offset == Pos) {
          // 使用其他变量进行初始化
          printLn("  # %s全局变量", Var->Name);
          printLn("  .quad %s%+ld", Rel->Label, Rel->Addend);
          Rel = Rel->Next;
          Pos += 8;
        } else {
          // 打印出字符串的内容，包括转义字符
          printLn("  # 字符串字面量");
          char C = Var->InitData[Pos++];
          if (isprint(C))
            printLn("  .byte %d\t# %c", C, C);
          else
            printLn("  .byte %d", C);
        }
      }
      continue;
    }

    // bss段未给数据分配空间，只记录数据所需空间的大小
    printLn("  # 未初始化的全局变量");
    printLn("  .bss");
    printLn("%s:", Var->Name);
    printLn("  # 全局变量零填充%d位", Var->Ty->Size);
    printLn("  .zero %d", Var->Ty->Size);
  }
}

// 将浮点寄存器的值存入栈中
static void storeFloat(int Reg, int Offset, int Sz) {
  printLn("  # 将fa%d寄存器的值存入%d(fp)的栈地址", Reg, Offset);
  printLn("  li t0, %d", Offset);
  printLn("  add t0, fp, t0");

  switch (Sz) {
  case 4:
    printLn("  fsw fa%d, 0(t0)", Reg);
    return;
  case 8:
    printLn("  fsd fa%d, 0(t0)", Reg);
    return;
  default:
    unreachable();
  }
}

// 将整形寄存器的值存入栈中
static void storeGeneral(int Reg, int Offset, int Size) {
  printLn("  # 将a%d寄存器的值存入%d(fp)的栈地址", Reg, Offset);
  printLn("  li t0, %d", Offset);
  printLn("  add t0, fp, t0");
  switch (Size) {
  case 1:
    printLn("  sb a%d, 0(t0)", Reg);
    return;
  case 2:
    printLn("  sh a%d, 0(t0)", Reg);
    return;
  case 4:
    printLn("  sw a%d, 0(t0)", Reg);
    return;
  case 8:
    printLn("  sd a%d, 0(t0)", Reg);
    return;
  }
  unreachable();
}

// 代码生成入口函数，包含代码块的基础信息
void emitText(Obj *Prog) {
  // 为每个函数单独生成代码
  for (Obj *Fn = Prog; Fn; Fn = Fn->Next) {
    if (!Fn->IsFunction || !Fn->IsDefinition)
      continue;

    if (Fn->IsStatic) {
      printLn("\n  # 定义局部%s函数", Fn->Name);
      printLn("  .local %s", Fn->Name);
    } else {
      printLn("\n  # 定义全局%s函数", Fn->Name);
      printLn("  .globl %s", Fn->Name);
    }

    printLn("  # 代码段标签");
    printLn("  .text");
    printLn("# =====%s段开始===============", Fn->Name);
    printLn("# %s段标签", Fn->Name);
    printLn("%s:", Fn->Name);
    CurrentFn = Fn;

    // 栈布局
    //-------------------------------// sp
    //              ra
    //-------------------------------// ra = sp-8
    //              fp
    //-------------------------------// fp = sp-16
    //             变量
    //-------------------------------// sp = sp-16-StackSize
    //           表达式计算
    //-------------------------------//

    // Prologue, 前言
    // 将ra寄存器压栈,保存ra的值
    printLn("  # 将ra寄存器压栈,保存ra的值");
    printLn("  addi sp, sp, -16");
    printLn("  sd ra, 8(sp)");
    // 将fp压入栈中，保存fp的值
    printLn("  # 将fp压栈，fp属于“被调用者保存”的寄存器，需要恢复原值");
    printLn("  sd fp, 0(sp)");
    // 将sp写入fp
    printLn("  # 将sp的值写入fp");
    printLn("  mv fp, sp");

    // 偏移量为实际变量所用的栈大小
    printLn("  # sp腾出StackSize大小的栈空间");
    printLn("  li t0, -%d", Fn->StackSize);
    printLn("  add sp, sp, t0");

    // 记录整型寄存器，浮点寄存器使用的数量
    int GP = 0, FP = 0;
    for (Obj *Var = Fn->Params; Var; Var = Var->Next) {
      if (Var->Offset > 0) {
        continue;
      }

      Type *Ty = Var->Ty;
      if (Ty->Kind != TY_ARRAY) {
        // 正常传递的形参
        switch (Ty->Kind) {
        case TY_STRUCT:
        case TY_UNION: {
          int N = 0;
          bool HasFloat = false;
          for (Member *Mem = Ty->Mems; Mem; Mem = Mem->Next) {
            N++;
            if (isFloNum(Mem->Ty))
              HasFloat = true;
          }

          // 结构体的栈内偏移量
          int StructOffset = Var->Offset;

          // struct只有一个浮点寄存器
          if (N == 1 && HasFloat) {
            printLn("  # 一个浮点寄存器传递的struct");
            storeFloat(FP++, StructOffset, Var->Ty->Size);
            break;
          }

          // struct有一个或两个浮点寄存器
          if (N == 2 && HasFloat) {
            printLn("  # 一个或两个浮点寄存器传递的struct");
            Member *Mem1 = Ty->Mems;
            Member *Mem2 = Mem1->Next;

            if (isFloNum(Mem1->Ty))
              storeFloat(FP++, StructOffset, Mem1->Ty->Size);
            else
              storeGeneral(GP++, StructOffset, Mem1->Ty->Size);

            if (isFloNum(Mem2->Ty))
              storeFloat(FP++, StructOffset + Mem2->Offset, Mem2->Ty->Size);
            else
              storeGeneral(GP++, StructOffset + Mem2->Offset, Mem2->Ty->Size);
            break;
          }

          // 一个整型寄存器传递的struct
          if (Ty->Size <= 8) {
            printLn("  # 一个寄存器传递的struct");
            storeGeneral(GP++, StructOffset, Var->Ty->Size);
            break;
          }

          // 两个整型寄存器传递的struct
          if (Ty->Size <= 16) {
            printLn("  # 两个寄存器传递的struct");
            // 第一个寄存器
            storeGeneral(GP++, StructOffset, 8);
            // 第二个寄存器，偏移量为8，然后减去上一个寄存器占用的8个字节
            storeGeneral(GP++, StructOffset + 8, Var->Ty->Size - 8);
            break;
          }

          // 栈传递的struct
          // printLn("  # 栈传递的struct地址");
          // for (Member *Mem = Ty->Mems; Mem; Mem = Mem->Next) {
          //   if (isFloNum(Mem->Ty))
          //     storeFloat(FP++, StructOffset + Mem->Offset, Mem->Ty->Size);
          //   else
          //     storeGeneral(GP++, StructOffset + Mem->Offset, Mem->Ty->Size);
          // }
          // break;

          // TODO：从栈中加载struct的地址，然后逐个加载到栈中}
        }
        case TY_FLOAT:
        case TY_DOUBLE: {
          // 正常传递的浮点形参
          if (FP < FP_MAX) {
            printLn("  # 将浮点形参%s的浮点寄存器fa%d的值压栈", Var->Name, FP);
            storeFloat(FP++, Var->Offset, Var->Ty->Size);
          } else {
            printLn("  # 将浮点形参%s的整型寄存器a%d的值压栈", Var->Name, GP);
            storeGeneral(GP++, Var->Offset, Var->Ty->Size);
          }
          break;
        }
        default:
          // 正常传递的整型形参
          printLn("  # 正常传递的整型形参");
          storeGeneral(GP++, Var->Offset, Var->Ty->Size);
          break;
        }
      } else {
        // 可变参数存入__va_area__，注意最多为7个
        int Offset = Var->Offset;
        while (GP < GP_MAX) {
          printLn("  # 可变参数，相对%s的偏移量为%d", Var->Name,
                  Offset - Var->Offset);
          storeGeneral(GP++, Offset, 8);
          Offset += 8;
        }
      }
    }

    // 生成语句链表的代码
    printLn("# =====%s段主体===============", Fn->Name);
    genStmt(Fn->Body);
    assert(Depth == 0);

    // Epilogue，后语
    // 输出return段标签
    printLn("# =====%s段结束===============", Fn->Name);
    printLn("# return段标签");
    printLn(".L.return.%s:", Fn->Name);
    // 将fp的值改写回sp
    printLn("  # 将fp的值写回sp");
    printLn("  mv sp, fp");
    // 将最早fp保存的值弹栈，恢复fp。
    printLn("  # 将最早fp保存的值弹栈，恢复fp和sp");
    printLn("  ld fp, 0(sp)");
    // 将ra寄存器弹栈,恢复ra的值
    printLn("  # 将ra寄存器弹栈,恢复ra的值");
    printLn("  ld ra, 8(sp)");
    printLn("  addi sp, sp, 16");
    // 返回
    printLn("  # 返回a0值给系统调用");
    printLn("  ret");
  }
}

void codegen(Obj *Prog, FILE *Out) {
  // 设置目标文件的文件流指针
  OutputFile = Out;

  File **Files = getInputFiles();
  for (int I = 0; Files[I]; I++)
    printLn("  .file %d \"%s\"", Files[I]->FileNo, Files[I]->Name);

  // 计算局部变量的偏移量
  assignLVarOffsets(Prog);
  // 生成数据
  emitData(Prog);
  // 生成代码
  emitText(Prog);
}
