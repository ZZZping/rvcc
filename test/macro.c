// [179] 使用内建的预处理器用于所有测试
#include "test.h"

// [160] 支持 #include "..."
#include "include1.h"

#

/* */ #

// [172] 支持 #define 零参仿函数
int ret3(void) { return 3; }

// [176] 宏中终结符只展开一次
int dbl(int x) { return x*x; }

int main() {
  // [160] 支持 #include "..."
  ASSERT(5, include1);
  ASSERT(7, include2);

#if 0
#include "/no/such/file"
  ASSERT(0, 1);
#if nested
#endif
#endif

  int m = 0;

#if 1
  m = 5;
#endif
  ASSERT(5, m);

// [165] 支持 #else
#if 1
# if 0
#  if 1
    foo bar
#  endif
# endif
      m = 3;
#endif
    ASSERT(3, m);

#if 1-1
# if 1
# endif
# if 1
# else
# endif
# if 0
# else
# endif
  m = 2;
#else
# if 1
  m = 3;
# endif
#endif
  ASSERT(3, m);

#if 1
  m = 2;
#else
  m = 3;
#endif
  ASSERT(2, m);

// [166] 支持 #elif
#if 1
  m = 2;
#else
  m = 3;
#endif
  ASSERT(2, m);

#if 0
  m = 1;
#elif 0
  m = 2;
#elif 3+5
  m = 3;
#elif 1*5
  m = 4;
#endif
  ASSERT(3, m);

#if 1+5
  m = 1;
#elif 1
  m = 2;
#elif 3
  m = 2;
#endif
  ASSERT(1, m);

#if 0
  m = 1;
#elif 1
# if 1
  m = 2;
# else
  m = 3;
# endif
#else
  m = 5;
#endif
  ASSERT(2, m);

  // [167] 支持 #define
  int M1 = 5;

#define M1 3
  ASSERT(3, M1);
#define M1 4
  ASSERT(4, M1);

#define M1 3+4+
  ASSERT(12, M1 5);

#define M1 3+4
  ASSERT(23, M1*5);

#define ASSERT_ assert(
#define if 5
#define five "5"
#define END )
  ASSERT_ 5, if, five END;

// [168] 支持 #undef
#undef ASSERT_
#undef if
#undef five
#undef END

  if (0);

// [169] 展开 #if 和 #elif 中的参数
#define M 5
#if M
  m = 5;
#else
  m = 6;
#endif
  ASSERT(5, m);

#define M 5
#if M-5
  m = 6;
#elif M
  m = 5;
#endif
  ASSERT(5, m);

  // [170] 宏中只展开一次
  int M2 = 6;
#define M2 M2 + 3
  ASSERT(9, M2);

#define M3 M2 + 3
  ASSERT(12, M3);

  int M4 = 3;
#define M4 M5 * 5
#define M5 M4 + 2
  ASSERT(13, M4);

// [171] 支持 #ifdef 和 #idndef
#ifdef M6
  m = 5;
#else
  m = 3;
#endif
  ASSERT(3, m);

#define M6
#ifdef M6
  m = 5;
#else
  m = 3;
#endif
  ASSERT(5, m);

#ifndef M7
  m = 3;
#else
  m = 5;
#endif
  ASSERT(3, m);

#define M7
#ifndef M7
  m = 3;
#else
  m = 5;
#endif
  ASSERT(5, m);

#if 0
#ifdef NO_SUCH_MACRO
#endif
#ifndef NO_SUCH_MACRO
#endif
#else
#endif

// [172] 支持 #define 零参仿函数
#define M7() 1
  int M7 = 5;
  ASSERT(1, M7());
  ASSERT(5, M7);

#define M7 ()
  ASSERT(3, ret3 M7);

// [173] 支持 #define 多参仿函数
#define M8(x,y) x+y
  ASSERT(7, M8(3, 4));

#define M8(x,y) x*y
  ASSERT(24, M8(3+4, 4+5));

#define M8(x,y) (x)*(y)
  ASSERT(63, M8(3+4, 4+5));

// [174] 支持空的宏参数
#define M8(x,y) x y
  ASSERT(9, M8(, 4+5));

#define M8(x,y) x*y
  ASSERT(20, M8((2+3), 4));

// [175] 允许括号内的表达式作为宏参数
#define M8(x,y) x*y
  ASSERT(12, M8((2,3), 4));

// [176] 宏中终结符只展开一次
#define dbl(x) M10(x) * x
#define M10(x) dbl(x) + 3
  ASSERT(10, dbl(2));

#define M11(x) #x
  ASSERT('a', M11( a!b  `""c)[0]);
  ASSERT('!', M11( a!b  `""c)[1]);
  ASSERT('b', M11( a!b  `""c)[2]);
  ASSERT(' ', M11( a!b  `""c)[3]);
  ASSERT('`', M11( a!b  `""c)[4]);
  ASSERT('"', M11( a!b  `""c)[5]);
  ASSERT('"', M11( a!b  `""c)[6]);
  ASSERT('c', M11( a!b  `""c)[7]);
  ASSERT(0, M11( a!b  `""c)[8]);

// [178] 支持宏 ## 操作符
#define paste(x,y) x##y
  ASSERT(15, paste(1,5));
  ASSERT(255, paste(0,xff));
  ASSERT(3, ({ int foobar=3; paste(foo,bar); }));
  ASSERT(5, paste(5,));
  ASSERT(5, paste(,5));

#define i 5
  ASSERT(101, ({ int i3=100; paste(1+i,3); }));
#undef i

#define paste2(x) x##5
  ASSERT(26, paste2(1+2));

#define paste3(x) 2##x
  ASSERT(23, paste3(1+2));

#define paste4(x, y, z) x##y##z
  ASSERT(123, paste4(1,2,3));

  // [159] 支持空指示
  printf("OK\n");
  return 0;
}
