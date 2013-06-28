/*
 * COPYRIGHT AND PERMISSION NOTICE
 * 
 * Copyright (c) 2003 Embedded Unit Project
 * 
 * All rights reserved.
 * 
 * Permission is hereby granted, free of charge, to any person obtaining 
 * a copy of this software and associated documentation files (the 
 * "Software"), to deal in the Software without restriction, including 
 * without limitation the rights to use, copy, modify, merge, publish, 
 * distribute, and/or sell copies of the Software, and to permit persons 
 * to whom the Software is furnished to do so, provided that the above 
 * copyright notice(s) and this permission notice appear in all copies 
 * of the Software and that both the above copyright notice(s) and this 
 * permission notice appear in supporting documentation.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, 
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF 
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT 
 * OF THIRD PARTY RIGHTS. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR 
 * HOLDERS INCLUDED IN THIS NOTICE BE LIABLE FOR ANY CLAIM, OR ANY 
 * SPECIAL INDIRECT OR CONSEQUENTIAL DAMAGES, OR ANY DAMAGES WHATSOEVER 
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF 
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN 
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 * 
 * Except as contained in this notice, the name of a copyright holder 
 * shall not be used in advertising or otherwise to promote the sale, 
 * use or other dealings in this Software without prior written 
 * authorization of the copyright holder.
 *
 * $Id: AssertImpl.h,v 1.6 2003/09/16 11:09:53 arms22 Exp $
 */
#ifndef	__ASSERTIMPL_H__
#define	__ASSERTIMPL_H__
#include <linux/types.h>
#ifdef	__cplusplus
extern "C" {
#endif

void addFailure(const char *msg, long line, const char *file);	/*TestCase.c*/

void assertImplementationInt(int expected,int actual, long line, const char *file);
void assertImplementationCStr(const char *expected,const char *actual, long line, const char *file);

void assert_equal_u8(u8 expected, u8 actual, long line, const char *file);
void assert_equal_u64(u64 expected, u64 actual, long line, const char *file);
void assert_equal_ulong_val(unsigned long expected, unsigned long actual, 
    long line, const char *file);
void assert_equal_ulong_ptr(unsigned long expected, unsigned long actual, 
    long line, const char *file);
void assert_equal_int(int expected, int actual, long line, const char *file);
void assert_equal_char(char expected, char actual, long line, const char *file);
void assert_equal_ptr(void *expected, void *actual, long line, const char *file);
void mark_failed_assertion(long line, const char *file);

#define TEST_ASSERT_EQUAL_U8(expected,actual,msg, ...)\
    do { \
      u8 ev = (expected); \
      u8 av = (actual); \
      if (ev == av) {} \
      else {  \
        assert_equal_u8(ev, av, __LINE__, __FILE__); \
        printk(msg, ##__VA_ARGS__);return;} \
    } while (0);

#define TEST_ASSERT_EQUAL_U64(expected,actual,msg, ...)\
    do { \
      u64 ev = (expected); \
      u64 av = (actual); \
      if (ev == av) {} \
      else {  \
        assert_equal_u64(ev, av, __LINE__, __FILE__); \
        printk(msg, ##__VA_ARGS__);return;} \
    } while (0);

#define TEST_ASSERT_EQUAL_ULONG_VAL(expected,actual,msg, ...)\
    do { \
      unsigned long ev = (expected); \
      unsigned long av = (actual); \
      if (ev == av) {} \
      else {  \
        assert_equal_ulong_val(ev, av, __LINE__, __FILE__); \
        printk(msg, ##__VA_ARGS__);return;} \
    } while (0);


#define TEST_ASSERT_EQUAL_ULONG_PTR(expected,actual,msg, ...)\
    do { \
      unsigned long ev = (expected); \
      unsigned long av = (actual); \
      if (ev == av) {} \
      else {  \
        assert_equal_ulong_ptr(ev, av, __LINE__, __FILE__); \
        printk(msg, ##__VA_ARGS__);return;} \
    } while (0);

#define TEST_ASSERT_EQUAL_INT(expected,actual,msg, ...)\
    do { \
      int ev = (expected); \
      int av = (actual); \
      if (ev == av) {} \
      else {  \
        assert_equal_int(ev, av, __LINE__, __FILE__); \
        printk(msg, ##__VA_ARGS__);return;} \
    } while (0);

#define TEST_ASSERT_EQUAL_CHAR(expected,actual,msg, ...)\
    do { \
      char ev = (expected); \
      char av = (actual); \
      if (ev == av) {} \
      else {  \
        assert_equal_char(ev, av, __LINE__, __FILE__); \
        printk(msg, ##__VA_ARGS__);return;} \
    } while (0);

#define TEST_ASSERT_EQUAL_PTR(expected,actual,msg, ...)\
    do { \
      void *ev = (expected); \
      void *av = (actual); \
      if (ev == av) {} \
      else {  \
        assert_equal_ptr(ev, av, __LINE__, __FILE__); \
        printk(msg, ##__VA_ARGS__);return;} \
    } while (0);

#define TEST_ASSERT_TRUE(condition, msg, ...) \
    do { \
      if(condition) {} \
      else { \
        printk("condition failed: %s\n\t", #condition); \
        printk(msg, ##__VA_ARGS__); \
        mark_failed_assertion(__LINE__, __FILE__);return;} \
    } while (0);

#define TEST_ASSERT_EQUAL_STRING(expected,actual)\
	if (expected && actual && (stdimpl_strcmp(expected,actual)==0)) {} else {assertImplementationCStr(expected,actual,__LINE__,__FILE__);return;}

#define OLD_TEST_ASSERT_EQUAL_INT(expected,actual)\
	if (expected == actual) {} else {assertImplementationInt(expected,actual,__LINE__,__FILE__);return;}

#define TEST_ASSERT_EQUAL_MSG(expected,actual,msg)\
	if (expected == actual) {} else {assertImplementationInt(expected,actual,__LINE__,__FILE__);printk("%s:%lu - %s\n",__FILE__,__LINE__,msg);return;}

#define TEST_ASSERT_NULL(pointer)\
	TEST_ASSERT_MESSAGE(pointer == NULL,#pointer " was not null.")

#define TEST_ASSERT_NOT_NULL(pointer)\
	TEST_ASSERT_MESSAGE(pointer != NULL,#pointer " was null.")

#define TEST_ASSERT_MESSAGE(condition, message)\
	if (condition) {} else {TEST_FAIL(message);}

#define TEST_ASSERT(condition)\
	if (condition) {} else {TEST_FAIL(#condition);}

#define TEST_FAIL(message)\
	if (0) {} else {addFailure(message,__LINE__,__FILE__);return;}

#ifdef	__cplusplus
}
#endif

#endif/*__ASSERTIMPL_H__*/
