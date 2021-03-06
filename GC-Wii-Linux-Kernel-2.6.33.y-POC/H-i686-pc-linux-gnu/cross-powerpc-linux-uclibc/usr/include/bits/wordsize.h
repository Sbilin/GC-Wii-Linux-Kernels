/* Determine the wordsize from the preprocessor defines.  */

#if defined __powerpc64__
# define __WORDSIZE	64
# define __WORDSIZE_COMPAT32	1
#else
# define __WORDSIZE	32
#endif

#if defined __UCLIBC_HAS_LONG_DOUBLE_MATH__ && !defined __LONG_DOUBLE_MATH_OPTIONAL

/* Signal the glibc ABI didn't used to have a `long double'.
   The changes all the `long double' function variants to be redirects
   to the double functions.  */
# define __LONG_DOUBLE_MATH_OPTIONAL   1
# ifndef __LONG_DOUBLE_128__
#  undef __UCLIBC_HAS_LONG_DOUBLE_MATH__
# endif
#endif
