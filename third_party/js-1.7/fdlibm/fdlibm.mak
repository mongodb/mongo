# Microsoft Developer Studio Generated NMAKE File, Format Version 4.20
# ** DO NOT EDIT **

# TARGTYPE "Win32 (x86) Static Library" 0x0104

!IF "$(CFG)" == ""
CFG=fdlibm - Win32 Debug
!MESSAGE No configuration specified.  Defaulting to fdlibm - Win32 Debug.
!ENDIF 

!IF "$(CFG)" != "fdlibm - Win32 Release" && "$(CFG)" != "fdlibm - Win32 Debug"
!MESSAGE Invalid configuration "$(CFG)" specified.
!MESSAGE You can specify a configuration when running NMAKE on this makefile
!MESSAGE by defining the macro CFG on the command line.  For example:
!MESSAGE 
!MESSAGE NMAKE /f "fdlibm.mak" CFG="fdlibm - Win32 Debug"
!MESSAGE 
!MESSAGE Possible choices for configuration are:
!MESSAGE 
!MESSAGE "fdlibm - Win32 Release" (based on "Win32 (x86) Static Library")
!MESSAGE "fdlibm - Win32 Debug" (based on "Win32 (x86) Static Library")
!MESSAGE 
!ERROR An invalid configuration is specified.
!ENDIF 

!IF "$(OS)" == "Windows_NT"
NULL=
!ELSE 
NULL=nul
!ENDIF 
################################################################################
# Begin Project
CPP=cl.exe

!IF  "$(CFG)" == "fdlibm - Win32 Release"

# PROP BASE Use_MFC 0
# PROP BASE Use_Debug_Libraries 0
# PROP BASE Output_Dir "fdlibm__"
# PROP BASE Intermediate_Dir "fdlibm__"
# PROP BASE Target_Dir ""
# PROP Use_MFC 0
# PROP Use_Debug_Libraries 0
# PROP Output_Dir "fdlibm__"
# PROP Intermediate_Dir "fdlibm__"
# PROP Target_Dir ""
OUTDIR=.\fdlibm__
INTDIR=.\fdlibm__

ALL : "$(OUTDIR)\fdlibm.lib"

CLEAN : 
	-@erase "$(INTDIR)\e_acos.obj"
	-@erase "$(INTDIR)\e_acosh.obj"
	-@erase "$(INTDIR)\e_asin.obj"
	-@erase "$(INTDIR)\e_atan2.obj"
	-@erase "$(INTDIR)\e_atanh.obj"
	-@erase "$(INTDIR)\e_cosh.obj"
	-@erase "$(INTDIR)\e_exp.obj"
	-@erase "$(INTDIR)\e_fmod.obj"
	-@erase "$(INTDIR)\e_gamma.obj"
	-@erase "$(INTDIR)\e_gamma_r.obj"
	-@erase "$(INTDIR)\e_hypot.obj"
	-@erase "$(INTDIR)\e_j0.obj"
	-@erase "$(INTDIR)\e_j1.obj"
	-@erase "$(INTDIR)\e_jn.obj"
	-@erase "$(INTDIR)\e_lgamma.obj"
	-@erase "$(INTDIR)\e_lgamma_r.obj"
	-@erase "$(INTDIR)\e_log.obj"
	-@erase "$(INTDIR)\e_log10.obj"
	-@erase "$(INTDIR)\e_pow.obj"
	-@erase "$(INTDIR)\e_rem_pio2.obj"
	-@erase "$(INTDIR)\e_remainder.obj"
	-@erase "$(INTDIR)\e_scalb.obj"
	-@erase "$(INTDIR)\e_sinh.obj"
	-@erase "$(INTDIR)\e_sqrt.obj"
	-@erase "$(INTDIR)\k_cos.obj"
	-@erase "$(INTDIR)\k_rem_pio2.obj"
	-@erase "$(INTDIR)\k_sin.obj"
	-@erase "$(INTDIR)\k_standard.obj"
	-@erase "$(INTDIR)\k_tan.obj"
	-@erase "$(INTDIR)\s_asinh.obj"
	-@erase "$(INTDIR)\s_atan.obj"
	-@erase "$(INTDIR)\s_cbrt.obj"
	-@erase "$(INTDIR)\s_ceil.obj"
	-@erase "$(INTDIR)\s_copysign.obj"
	-@erase "$(INTDIR)\s_cos.obj"
	-@erase "$(INTDIR)\s_erf.obj"
	-@erase "$(INTDIR)\s_expm1.obj"
	-@erase "$(INTDIR)\s_fabs.obj"
	-@erase "$(INTDIR)\s_finite.obj"
	-@erase "$(INTDIR)\s_floor.obj"
	-@erase "$(INTDIR)\s_frexp.obj"
	-@erase "$(INTDIR)\s_ilogb.obj"
	-@erase "$(INTDIR)\s_isnan.obj"
	-@erase "$(INTDIR)\s_ldexp.obj"
	-@erase "$(INTDIR)\s_lib_version.obj"
	-@erase "$(INTDIR)\s_log1p.obj"
	-@erase "$(INTDIR)\s_logb.obj"
	-@erase "$(INTDIR)\s_matherr.obj"
	-@erase "$(INTDIR)\s_modf.obj"
	-@erase "$(INTDIR)\s_nextafter.obj"
	-@erase "$(INTDIR)\s_rint.obj"
	-@erase "$(INTDIR)\s_scalbn.obj"
	-@erase "$(INTDIR)\s_signgam.obj"
	-@erase "$(INTDIR)\s_significand.obj"
	-@erase "$(INTDIR)\s_sin.obj"
	-@erase "$(INTDIR)\s_tan.obj"
	-@erase "$(INTDIR)\s_tanh.obj"
	-@erase "$(INTDIR)\w_acos.obj"
	-@erase "$(INTDIR)\w_acosh.obj"
	-@erase "$(INTDIR)\w_asin.obj"
	-@erase "$(INTDIR)\w_atan2.obj"
	-@erase "$(INTDIR)\w_atanh.obj"
	-@erase "$(INTDIR)\w_cosh.obj"
	-@erase "$(INTDIR)\w_exp.obj"
	-@erase "$(INTDIR)\w_fmod.obj"
	-@erase "$(INTDIR)\w_gamma.obj"
	-@erase "$(INTDIR)\w_gamma_r.obj"
	-@erase "$(INTDIR)\w_hypot.obj"
	-@erase "$(INTDIR)\w_j0.obj"
	-@erase "$(INTDIR)\w_j1.obj"
	-@erase "$(INTDIR)\w_jn.obj"
	-@erase "$(INTDIR)\w_lgamma.obj"
	-@erase "$(INTDIR)\w_lgamma_r.obj"
	-@erase "$(INTDIR)\w_log.obj"
	-@erase "$(INTDIR)\w_log10.obj"
	-@erase "$(INTDIR)\w_pow.obj"
	-@erase "$(INTDIR)\w_remainder.obj"
	-@erase "$(INTDIR)\w_scalb.obj"
	-@erase "$(INTDIR)\w_sinh.obj"
	-@erase "$(INTDIR)\w_sqrt.obj"
	-@erase "$(OUTDIR)\fdlibm.lib"

"$(OUTDIR)" :
    if not exist "$(OUTDIR)/$(NULL)" mkdir "$(OUTDIR)"

# ADD BASE CPP /nologo /W3 /GX /O2 /D "WIN32" /D "NDEBUG" /D "_WINDOWS" /YX /c
# ADD CPP /nologo /W3 /GX /O2 /D "WIN32" /D "NDEBUG" /D "_WINDOWS" /YX /c
CPP_PROJ=/nologo /ML /W3 /GX /O2 /D "WIN32" /D "NDEBUG" /D "_WINDOWS"\
 /Fp"$(INTDIR)/fdlibm.pch" /YX /Fo"$(INTDIR)/" /c 
CPP_OBJS=.\fdlibm__/
CPP_SBRS=.\.
BSC32=bscmake.exe
# ADD BASE BSC32 /nologo
# ADD BSC32 /nologo
BSC32_FLAGS=/nologo /o"$(OUTDIR)/fdlibm.bsc" 
BSC32_SBRS= \
	
LIB32=link.exe -lib
# ADD BASE LIB32 /nologo
# ADD LIB32 /nologo
LIB32_FLAGS=/nologo /out:"$(OUTDIR)/fdlibm.lib" 
LIB32_OBJS= \
	"$(INTDIR)\e_acos.obj" \
	"$(INTDIR)\e_acosh.obj" \
	"$(INTDIR)\e_asin.obj" \
	"$(INTDIR)\e_atan2.obj" \
	"$(INTDIR)\e_atanh.obj" \
	"$(INTDIR)\e_cosh.obj" \
	"$(INTDIR)\e_exp.obj" \
	"$(INTDIR)\e_fmod.obj" \
	"$(INTDIR)\e_gamma.obj" \
	"$(INTDIR)\e_gamma_r.obj" \
	"$(INTDIR)\e_hypot.obj" \
	"$(INTDIR)\e_j0.obj" \
	"$(INTDIR)\e_j1.obj" \
	"$(INTDIR)\e_jn.obj" \
	"$(INTDIR)\e_lgamma.obj" \
	"$(INTDIR)\e_lgamma_r.obj" \
	"$(INTDIR)\e_log.obj" \
	"$(INTDIR)\e_log10.obj" \
	"$(INTDIR)\e_pow.obj" \
	"$(INTDIR)\e_rem_pio2.obj" \
	"$(INTDIR)\e_remainder.obj" \
	"$(INTDIR)\e_scalb.obj" \
	"$(INTDIR)\e_sinh.obj" \
	"$(INTDIR)\e_sqrt.obj" \
	"$(INTDIR)\k_cos.obj" \
	"$(INTDIR)\k_rem_pio2.obj" \
	"$(INTDIR)\k_sin.obj" \
	"$(INTDIR)\k_standard.obj" \
	"$(INTDIR)\k_tan.obj" \
	"$(INTDIR)\s_asinh.obj" \
	"$(INTDIR)\s_atan.obj" \
	"$(INTDIR)\s_cbrt.obj" \
	"$(INTDIR)\s_ceil.obj" \
	"$(INTDIR)\s_copysign.obj" \
	"$(INTDIR)\s_cos.obj" \
	"$(INTDIR)\s_erf.obj" \
	"$(INTDIR)\s_expm1.obj" \
	"$(INTDIR)\s_fabs.obj" \
	"$(INTDIR)\s_finite.obj" \
	"$(INTDIR)\s_floor.obj" \
	"$(INTDIR)\s_frexp.obj" \
	"$(INTDIR)\s_ilogb.obj" \
	"$(INTDIR)\s_isnan.obj" \
	"$(INTDIR)\s_ldexp.obj" \
	"$(INTDIR)\s_lib_version.obj" \
	"$(INTDIR)\s_log1p.obj" \
	"$(INTDIR)\s_logb.obj" \
	"$(INTDIR)\s_matherr.obj" \
	"$(INTDIR)\s_modf.obj" \
	"$(INTDIR)\s_nextafter.obj" \
	"$(INTDIR)\s_rint.obj" \
	"$(INTDIR)\s_scalbn.obj" \
	"$(INTDIR)\s_signgam.obj" \
	"$(INTDIR)\s_significand.obj" \
	"$(INTDIR)\s_sin.obj" \
	"$(INTDIR)\s_tan.obj" \
	"$(INTDIR)\s_tanh.obj" \
	"$(INTDIR)\w_acos.obj" \
	"$(INTDIR)\w_acosh.obj" \
	"$(INTDIR)\w_asin.obj" \
	"$(INTDIR)\w_atan2.obj" \
	"$(INTDIR)\w_atanh.obj" \
	"$(INTDIR)\w_cosh.obj" \
	"$(INTDIR)\w_exp.obj" \
	"$(INTDIR)\w_fmod.obj" \
	"$(INTDIR)\w_gamma.obj" \
	"$(INTDIR)\w_gamma_r.obj" \
	"$(INTDIR)\w_hypot.obj" \
	"$(INTDIR)\w_j0.obj" \
	"$(INTDIR)\w_j1.obj" \
	"$(INTDIR)\w_jn.obj" \
	"$(INTDIR)\w_lgamma.obj" \
	"$(INTDIR)\w_lgamma_r.obj" \
	"$(INTDIR)\w_log.obj" \
	"$(INTDIR)\w_log10.obj" \
	"$(INTDIR)\w_pow.obj" \
	"$(INTDIR)\w_remainder.obj" \
	"$(INTDIR)\w_scalb.obj" \
	"$(INTDIR)\w_sinh.obj" \
	"$(INTDIR)\w_sqrt.obj"

"$(OUTDIR)\fdlibm.lib" : "$(OUTDIR)" $(DEF_FILE) $(LIB32_OBJS)
    $(LIB32) @<<
  $(LIB32_FLAGS) $(DEF_FLAGS) $(LIB32_OBJS)
<<

!ELSEIF  "$(CFG)" == "fdlibm - Win32 Debug"

# PROP BASE Use_MFC 0
# PROP BASE Use_Debug_Libraries 1
# PROP BASE Output_Dir "fdlibm_0"
# PROP BASE Intermediate_Dir "fdlibm_0"
# PROP BASE Target_Dir ""
# PROP Use_MFC 0
# PROP Use_Debug_Libraries 1
# PROP Output_Dir "fdlibm_0"
# PROP Intermediate_Dir "fdlibm_0"
# PROP Target_Dir ""
OUTDIR=.\fdlibm_0
INTDIR=.\fdlibm_0

ALL : "$(OUTDIR)\fdlibm.lib"

CLEAN : 
	-@erase "$(INTDIR)\e_acos.obj"
	-@erase "$(INTDIR)\e_acosh.obj"
	-@erase "$(INTDIR)\e_asin.obj"
	-@erase "$(INTDIR)\e_atan2.obj"
	-@erase "$(INTDIR)\e_atanh.obj"
	-@erase "$(INTDIR)\e_cosh.obj"
	-@erase "$(INTDIR)\e_exp.obj"
	-@erase "$(INTDIR)\e_fmod.obj"
	-@erase "$(INTDIR)\e_gamma.obj"
	-@erase "$(INTDIR)\e_gamma_r.obj"
	-@erase "$(INTDIR)\e_hypot.obj"
	-@erase "$(INTDIR)\e_j0.obj"
	-@erase "$(INTDIR)\e_j1.obj"
	-@erase "$(INTDIR)\e_jn.obj"
	-@erase "$(INTDIR)\e_lgamma.obj"
	-@erase "$(INTDIR)\e_lgamma_r.obj"
	-@erase "$(INTDIR)\e_log.obj"
	-@erase "$(INTDIR)\e_log10.obj"
	-@erase "$(INTDIR)\e_pow.obj"
	-@erase "$(INTDIR)\e_rem_pio2.obj"
	-@erase "$(INTDIR)\e_remainder.obj"
	-@erase "$(INTDIR)\e_scalb.obj"
	-@erase "$(INTDIR)\e_sinh.obj"
	-@erase "$(INTDIR)\e_sqrt.obj"
	-@erase "$(INTDIR)\k_cos.obj"
	-@erase "$(INTDIR)\k_rem_pio2.obj"
	-@erase "$(INTDIR)\k_sin.obj"
	-@erase "$(INTDIR)\k_standard.obj"
	-@erase "$(INTDIR)\k_tan.obj"
	-@erase "$(INTDIR)\s_asinh.obj"
	-@erase "$(INTDIR)\s_atan.obj"
	-@erase "$(INTDIR)\s_cbrt.obj"
	-@erase "$(INTDIR)\s_ceil.obj"
	-@erase "$(INTDIR)\s_copysign.obj"
	-@erase "$(INTDIR)\s_cos.obj"
	-@erase "$(INTDIR)\s_erf.obj"
	-@erase "$(INTDIR)\s_expm1.obj"
	-@erase "$(INTDIR)\s_fabs.obj"
	-@erase "$(INTDIR)\s_finite.obj"
	-@erase "$(INTDIR)\s_floor.obj"
	-@erase "$(INTDIR)\s_frexp.obj"
	-@erase "$(INTDIR)\s_ilogb.obj"
	-@erase "$(INTDIR)\s_isnan.obj"
	-@erase "$(INTDIR)\s_ldexp.obj"
	-@erase "$(INTDIR)\s_lib_version.obj"
	-@erase "$(INTDIR)\s_log1p.obj"
	-@erase "$(INTDIR)\s_logb.obj"
	-@erase "$(INTDIR)\s_matherr.obj"
	-@erase "$(INTDIR)\s_modf.obj"
	-@erase "$(INTDIR)\s_nextafter.obj"
	-@erase "$(INTDIR)\s_rint.obj"
	-@erase "$(INTDIR)\s_scalbn.obj"
	-@erase "$(INTDIR)\s_signgam.obj"
	-@erase "$(INTDIR)\s_significand.obj"
	-@erase "$(INTDIR)\s_sin.obj"
	-@erase "$(INTDIR)\s_tan.obj"
	-@erase "$(INTDIR)\s_tanh.obj"
	-@erase "$(INTDIR)\w_acos.obj"
	-@erase "$(INTDIR)\w_acosh.obj"
	-@erase "$(INTDIR)\w_asin.obj"
	-@erase "$(INTDIR)\w_atan2.obj"
	-@erase "$(INTDIR)\w_atanh.obj"
	-@erase "$(INTDIR)\w_cosh.obj"
	-@erase "$(INTDIR)\w_exp.obj"
	-@erase "$(INTDIR)\w_fmod.obj"
	-@erase "$(INTDIR)\w_gamma.obj"
	-@erase "$(INTDIR)\w_gamma_r.obj"
	-@erase "$(INTDIR)\w_hypot.obj"
	-@erase "$(INTDIR)\w_j0.obj"
	-@erase "$(INTDIR)\w_j1.obj"
	-@erase "$(INTDIR)\w_jn.obj"
	-@erase "$(INTDIR)\w_lgamma.obj"
	-@erase "$(INTDIR)\w_lgamma_r.obj"
	-@erase "$(INTDIR)\w_log.obj"
	-@erase "$(INTDIR)\w_log10.obj"
	-@erase "$(INTDIR)\w_pow.obj"
	-@erase "$(INTDIR)\w_remainder.obj"
	-@erase "$(INTDIR)\w_scalb.obj"
	-@erase "$(INTDIR)\w_sinh.obj"
	-@erase "$(INTDIR)\w_sqrt.obj"
	-@erase "$(OUTDIR)\fdlibm.lib"

"$(OUTDIR)" :
    if not exist "$(OUTDIR)/$(NULL)" mkdir "$(OUTDIR)"

# ADD BASE CPP /nologo /W3 /GX /Z7 /Od /D "WIN32" /D "_DEBUG" /D "_WINDOWS" /YX /c
# ADD CPP /nologo /W3 /GX /Z7 /Od /D "WIN32" /D "_DEBUG" /D "_WINDOWS" /YX /c
CPP_PROJ=/nologo /MLd /W3 /GX /Z7 /Od /D "WIN32" /D "_DEBUG" /D "_WINDOWS"\
 /Fp"$(INTDIR)/fdlibm.pch" /YX /Fo"$(INTDIR)/" /c 
CPP_OBJS=.\fdlibm_0/
CPP_SBRS=.\.
BSC32=bscmake.exe
# ADD BASE BSC32 /nologo
# ADD BSC32 /nologo
BSC32_FLAGS=/nologo /o"$(OUTDIR)/fdlibm.bsc" 
BSC32_SBRS= \
	
LIB32=link.exe -lib
# ADD BASE LIB32 /nologo
# ADD LIB32 /nologo
LIB32_FLAGS=/nologo /out:"$(OUTDIR)/fdlibm.lib" 
LIB32_OBJS= \
	"$(INTDIR)\e_acos.obj" \
	"$(INTDIR)\e_acosh.obj" \
	"$(INTDIR)\e_asin.obj" \
	"$(INTDIR)\e_atan2.obj" \
	"$(INTDIR)\e_atanh.obj" \
	"$(INTDIR)\e_cosh.obj" \
	"$(INTDIR)\e_exp.obj" \
	"$(INTDIR)\e_fmod.obj" \
	"$(INTDIR)\e_gamma.obj" \
	"$(INTDIR)\e_gamma_r.obj" \
	"$(INTDIR)\e_hypot.obj" \
	"$(INTDIR)\e_j0.obj" \
	"$(INTDIR)\e_j1.obj" \
	"$(INTDIR)\e_jn.obj" \
	"$(INTDIR)\e_lgamma.obj" \
	"$(INTDIR)\e_lgamma_r.obj" \
	"$(INTDIR)\e_log.obj" \
	"$(INTDIR)\e_log10.obj" \
	"$(INTDIR)\e_pow.obj" \
	"$(INTDIR)\e_rem_pio2.obj" \
	"$(INTDIR)\e_remainder.obj" \
	"$(INTDIR)\e_scalb.obj" \
	"$(INTDIR)\e_sinh.obj" \
	"$(INTDIR)\e_sqrt.obj" \
	"$(INTDIR)\k_cos.obj" \
	"$(INTDIR)\k_rem_pio2.obj" \
	"$(INTDIR)\k_sin.obj" \
	"$(INTDIR)\k_standard.obj" \
	"$(INTDIR)\k_tan.obj" \
	"$(INTDIR)\s_asinh.obj" \
	"$(INTDIR)\s_atan.obj" \
	"$(INTDIR)\s_cbrt.obj" \
	"$(INTDIR)\s_ceil.obj" \
	"$(INTDIR)\s_copysign.obj" \
	"$(INTDIR)\s_cos.obj" \
	"$(INTDIR)\s_erf.obj" \
	"$(INTDIR)\s_expm1.obj" \
	"$(INTDIR)\s_fabs.obj" \
	"$(INTDIR)\s_finite.obj" \
	"$(INTDIR)\s_floor.obj" \
	"$(INTDIR)\s_frexp.obj" \
	"$(INTDIR)\s_ilogb.obj" \
	"$(INTDIR)\s_isnan.obj" \
	"$(INTDIR)\s_ldexp.obj" \
	"$(INTDIR)\s_lib_version.obj" \
	"$(INTDIR)\s_log1p.obj" \
	"$(INTDIR)\s_logb.obj" \
	"$(INTDIR)\s_matherr.obj" \
	"$(INTDIR)\s_modf.obj" \
	"$(INTDIR)\s_nextafter.obj" \
	"$(INTDIR)\s_rint.obj" \
	"$(INTDIR)\s_scalbn.obj" \
	"$(INTDIR)\s_signgam.obj" \
	"$(INTDIR)\s_significand.obj" \
	"$(INTDIR)\s_sin.obj" \
	"$(INTDIR)\s_tan.obj" \
	"$(INTDIR)\s_tanh.obj" \
	"$(INTDIR)\w_acos.obj" \
	"$(INTDIR)\w_acosh.obj" \
	"$(INTDIR)\w_asin.obj" \
	"$(INTDIR)\w_atan2.obj" \
	"$(INTDIR)\w_atanh.obj" \
	"$(INTDIR)\w_cosh.obj" \
	"$(INTDIR)\w_exp.obj" \
	"$(INTDIR)\w_fmod.obj" \
	"$(INTDIR)\w_gamma.obj" \
	"$(INTDIR)\w_gamma_r.obj" \
	"$(INTDIR)\w_hypot.obj" \
	"$(INTDIR)\w_j0.obj" \
	"$(INTDIR)\w_j1.obj" \
	"$(INTDIR)\w_jn.obj" \
	"$(INTDIR)\w_lgamma.obj" \
	"$(INTDIR)\w_lgamma_r.obj" \
	"$(INTDIR)\w_log.obj" \
	"$(INTDIR)\w_log10.obj" \
	"$(INTDIR)\w_pow.obj" \
	"$(INTDIR)\w_remainder.obj" \
	"$(INTDIR)\w_scalb.obj" \
	"$(INTDIR)\w_sinh.obj" \
	"$(INTDIR)\w_sqrt.obj"

"$(OUTDIR)\fdlibm.lib" : "$(OUTDIR)" $(DEF_FILE) $(LIB32_OBJS)
    $(LIB32) @<<
  $(LIB32_FLAGS) $(DEF_FLAGS) $(LIB32_OBJS)
<<

!ENDIF 

.c{$(CPP_OBJS)}.obj:
   $(CPP) $(CPP_PROJ) $<  

.cpp{$(CPP_OBJS)}.obj:
   $(CPP) $(CPP_PROJ) $<  

.cxx{$(CPP_OBJS)}.obj:
   $(CPP) $(CPP_PROJ) $<  

.c{$(CPP_SBRS)}.sbr:
   $(CPP) $(CPP_PROJ) $<  

.cpp{$(CPP_SBRS)}.sbr:
   $(CPP) $(CPP_PROJ) $<  

.cxx{$(CPP_SBRS)}.sbr:
   $(CPP) $(CPP_PROJ) $<  

################################################################################
# Begin Target

# Name "fdlibm - Win32 Release"
# Name "fdlibm - Win32 Debug"

!IF  "$(CFG)" == "fdlibm - Win32 Release"

!ELSEIF  "$(CFG)" == "fdlibm - Win32 Debug"

!ENDIF 

################################################################################
# Begin Source File

SOURCE=.\w_sqrt.c
DEP_CPP_W_SQR=\
	".\fdlibm.h"\
	

"$(INTDIR)\w_sqrt.obj" : $(SOURCE) $(DEP_CPP_W_SQR) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\e_acosh.c
DEP_CPP_E_ACO=\
	".\fdlibm.h"\
	

"$(INTDIR)\e_acosh.obj" : $(SOURCE) $(DEP_CPP_E_ACO) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\e_asin.c
DEP_CPP_E_ASI=\
	".\fdlibm.h"\
	

"$(INTDIR)\e_asin.obj" : $(SOURCE) $(DEP_CPP_E_ASI) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\e_atan2.c
DEP_CPP_E_ATA=\
	".\fdlibm.h"\
	

"$(INTDIR)\e_atan2.obj" : $(SOURCE) $(DEP_CPP_E_ATA) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\e_atanh.c
DEP_CPP_E_ATAN=\
	".\fdlibm.h"\
	

"$(INTDIR)\e_atanh.obj" : $(SOURCE) $(DEP_CPP_E_ATAN) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\e_cosh.c
DEP_CPP_E_COS=\
	".\fdlibm.h"\
	

"$(INTDIR)\e_cosh.obj" : $(SOURCE) $(DEP_CPP_E_COS) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\e_exp.c
DEP_CPP_E_EXP=\
	".\fdlibm.h"\
	

"$(INTDIR)\e_exp.obj" : $(SOURCE) $(DEP_CPP_E_EXP) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\e_fmod.c
DEP_CPP_E_FMO=\
	".\fdlibm.h"\
	

"$(INTDIR)\e_fmod.obj" : $(SOURCE) $(DEP_CPP_E_FMO) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\e_gamma.c
DEP_CPP_E_GAM=\
	".\fdlibm.h"\
	

"$(INTDIR)\e_gamma.obj" : $(SOURCE) $(DEP_CPP_E_GAM) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\e_gamma_r.c
DEP_CPP_E_GAMM=\
	".\fdlibm.h"\
	

"$(INTDIR)\e_gamma_r.obj" : $(SOURCE) $(DEP_CPP_E_GAMM) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\e_hypot.c
DEP_CPP_E_HYP=\
	".\fdlibm.h"\
	

"$(INTDIR)\e_hypot.obj" : $(SOURCE) $(DEP_CPP_E_HYP) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\e_j0.c
DEP_CPP_E_J0_=\
	".\fdlibm.h"\
	

"$(INTDIR)\e_j0.obj" : $(SOURCE) $(DEP_CPP_E_J0_) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\e_j1.c
DEP_CPP_E_J1_=\
	".\fdlibm.h"\
	

"$(INTDIR)\e_j1.obj" : $(SOURCE) $(DEP_CPP_E_J1_) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\e_jn.c
DEP_CPP_E_JN_=\
	".\fdlibm.h"\
	

"$(INTDIR)\e_jn.obj" : $(SOURCE) $(DEP_CPP_E_JN_) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\e_lgamma.c
DEP_CPP_E_LGA=\
	".\fdlibm.h"\
	

"$(INTDIR)\e_lgamma.obj" : $(SOURCE) $(DEP_CPP_E_LGA) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\e_lgamma_r.c
DEP_CPP_E_LGAM=\
	".\fdlibm.h"\
	

"$(INTDIR)\e_lgamma_r.obj" : $(SOURCE) $(DEP_CPP_E_LGAM) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\e_log.c
DEP_CPP_E_LOG=\
	".\fdlibm.h"\
	

"$(INTDIR)\e_log.obj" : $(SOURCE) $(DEP_CPP_E_LOG) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\e_log10.c
DEP_CPP_E_LOG1=\
	".\fdlibm.h"\
	

"$(INTDIR)\e_log10.obj" : $(SOURCE) $(DEP_CPP_E_LOG1) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\e_pow.c
DEP_CPP_E_POW=\
	".\fdlibm.h"\
	

"$(INTDIR)\e_pow.obj" : $(SOURCE) $(DEP_CPP_E_POW) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\e_rem_pio2.c
DEP_CPP_E_REM=\
	".\fdlibm.h"\
	

"$(INTDIR)\e_rem_pio2.obj" : $(SOURCE) $(DEP_CPP_E_REM) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\e_remainder.c
DEP_CPP_E_REMA=\
	".\fdlibm.h"\
	

"$(INTDIR)\e_remainder.obj" : $(SOURCE) $(DEP_CPP_E_REMA) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\e_scalb.c
DEP_CPP_E_SCA=\
	".\fdlibm.h"\
	

"$(INTDIR)\e_scalb.obj" : $(SOURCE) $(DEP_CPP_E_SCA) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\e_sinh.c
DEP_CPP_E_SIN=\
	".\fdlibm.h"\
	

"$(INTDIR)\e_sinh.obj" : $(SOURCE) $(DEP_CPP_E_SIN) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\e_sqrt.c
DEP_CPP_E_SQR=\
	".\fdlibm.h"\
	

"$(INTDIR)\e_sqrt.obj" : $(SOURCE) $(DEP_CPP_E_SQR) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\fdlibm.h

!IF  "$(CFG)" == "fdlibm - Win32 Release"

!ELSEIF  "$(CFG)" == "fdlibm - Win32 Debug"

!ENDIF 

# End Source File
################################################################################
# Begin Source File

SOURCE=.\k_cos.c
DEP_CPP_K_COS=\
	".\fdlibm.h"\
	

"$(INTDIR)\k_cos.obj" : $(SOURCE) $(DEP_CPP_K_COS) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\k_rem_pio2.c
DEP_CPP_K_REM=\
	".\fdlibm.h"\
	

"$(INTDIR)\k_rem_pio2.obj" : $(SOURCE) $(DEP_CPP_K_REM) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\k_sin.c
DEP_CPP_K_SIN=\
	".\fdlibm.h"\
	

"$(INTDIR)\k_sin.obj" : $(SOURCE) $(DEP_CPP_K_SIN) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\k_standard.c
DEP_CPP_K_STA=\
	".\fdlibm.h"\
	

"$(INTDIR)\k_standard.obj" : $(SOURCE) $(DEP_CPP_K_STA) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\k_tan.c
DEP_CPP_K_TAN=\
	".\fdlibm.h"\
	

"$(INTDIR)\k_tan.obj" : $(SOURCE) $(DEP_CPP_K_TAN) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\s_asinh.c
DEP_CPP_S_ASI=\
	".\fdlibm.h"\
	

"$(INTDIR)\s_asinh.obj" : $(SOURCE) $(DEP_CPP_S_ASI) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\s_atan.c
DEP_CPP_S_ATA=\
	".\fdlibm.h"\
	

"$(INTDIR)\s_atan.obj" : $(SOURCE) $(DEP_CPP_S_ATA) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\s_cbrt.c
DEP_CPP_S_CBR=\
	".\fdlibm.h"\
	

"$(INTDIR)\s_cbrt.obj" : $(SOURCE) $(DEP_CPP_S_CBR) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\s_ceil.c
DEP_CPP_S_CEI=\
	".\fdlibm.h"\
	

"$(INTDIR)\s_ceil.obj" : $(SOURCE) $(DEP_CPP_S_CEI) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\s_copysign.c
DEP_CPP_S_COP=\
	".\fdlibm.h"\
	

"$(INTDIR)\s_copysign.obj" : $(SOURCE) $(DEP_CPP_S_COP) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\s_cos.c
DEP_CPP_S_COS=\
	".\fdlibm.h"\
	

"$(INTDIR)\s_cos.obj" : $(SOURCE) $(DEP_CPP_S_COS) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\s_erf.c
DEP_CPP_S_ERF=\
	".\fdlibm.h"\
	

"$(INTDIR)\s_erf.obj" : $(SOURCE) $(DEP_CPP_S_ERF) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\s_expm1.c
DEP_CPP_S_EXP=\
	".\fdlibm.h"\
	

"$(INTDIR)\s_expm1.obj" : $(SOURCE) $(DEP_CPP_S_EXP) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\s_fabs.c
DEP_CPP_S_FAB=\
	".\fdlibm.h"\
	

"$(INTDIR)\s_fabs.obj" : $(SOURCE) $(DEP_CPP_S_FAB) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\s_finite.c
DEP_CPP_S_FIN=\
	".\fdlibm.h"\
	

"$(INTDIR)\s_finite.obj" : $(SOURCE) $(DEP_CPP_S_FIN) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\s_floor.c
DEP_CPP_S_FLO=\
	".\fdlibm.h"\
	

"$(INTDIR)\s_floor.obj" : $(SOURCE) $(DEP_CPP_S_FLO) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\s_frexp.c
DEP_CPP_S_FRE=\
	".\fdlibm.h"\
	

"$(INTDIR)\s_frexp.obj" : $(SOURCE) $(DEP_CPP_S_FRE) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\s_ilogb.c
DEP_CPP_S_ILO=\
	".\fdlibm.h"\
	

"$(INTDIR)\s_ilogb.obj" : $(SOURCE) $(DEP_CPP_S_ILO) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\s_isnan.c
DEP_CPP_S_ISN=\
	".\fdlibm.h"\
	

"$(INTDIR)\s_isnan.obj" : $(SOURCE) $(DEP_CPP_S_ISN) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\s_ldexp.c
DEP_CPP_S_LDE=\
	".\fdlibm.h"\
	

"$(INTDIR)\s_ldexp.obj" : $(SOURCE) $(DEP_CPP_S_LDE) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\s_lib_version.c
DEP_CPP_S_LIB=\
	".\fdlibm.h"\
	

"$(INTDIR)\s_lib_version.obj" : $(SOURCE) $(DEP_CPP_S_LIB) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\s_log1p.c
DEP_CPP_S_LOG=\
	".\fdlibm.h"\
	

"$(INTDIR)\s_log1p.obj" : $(SOURCE) $(DEP_CPP_S_LOG) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\s_logb.c
DEP_CPP_S_LOGB=\
	".\fdlibm.h"\
	

"$(INTDIR)\s_logb.obj" : $(SOURCE) $(DEP_CPP_S_LOGB) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\s_matherr.c
DEP_CPP_S_MAT=\
	".\fdlibm.h"\
	

"$(INTDIR)\s_matherr.obj" : $(SOURCE) $(DEP_CPP_S_MAT) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\s_modf.c
DEP_CPP_S_MOD=\
	".\fdlibm.h"\
	

"$(INTDIR)\s_modf.obj" : $(SOURCE) $(DEP_CPP_S_MOD) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\s_nextafter.c
DEP_CPP_S_NEX=\
	".\fdlibm.h"\
	

"$(INTDIR)\s_nextafter.obj" : $(SOURCE) $(DEP_CPP_S_NEX) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\s_rint.c
DEP_CPP_S_RIN=\
	".\fdlibm.h"\
	

"$(INTDIR)\s_rint.obj" : $(SOURCE) $(DEP_CPP_S_RIN) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\s_scalbn.c
DEP_CPP_S_SCA=\
	".\fdlibm.h"\
	

"$(INTDIR)\s_scalbn.obj" : $(SOURCE) $(DEP_CPP_S_SCA) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\s_signgam.c
DEP_CPP_S_SIG=\
	".\fdlibm.h"\
	

"$(INTDIR)\s_signgam.obj" : $(SOURCE) $(DEP_CPP_S_SIG) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\s_significand.c
DEP_CPP_S_SIGN=\
	".\fdlibm.h"\
	

"$(INTDIR)\s_significand.obj" : $(SOURCE) $(DEP_CPP_S_SIGN) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\s_sin.c
DEP_CPP_S_SIN=\
	".\fdlibm.h"\
	

"$(INTDIR)\s_sin.obj" : $(SOURCE) $(DEP_CPP_S_SIN) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\s_tan.c
DEP_CPP_S_TAN=\
	".\fdlibm.h"\
	

"$(INTDIR)\s_tan.obj" : $(SOURCE) $(DEP_CPP_S_TAN) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\s_tanh.c
DEP_CPP_S_TANH=\
	".\fdlibm.h"\
	

"$(INTDIR)\s_tanh.obj" : $(SOURCE) $(DEP_CPP_S_TANH) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\w_acos.c
DEP_CPP_W_ACO=\
	".\fdlibm.h"\
	

"$(INTDIR)\w_acos.obj" : $(SOURCE) $(DEP_CPP_W_ACO) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\w_acosh.c
DEP_CPP_W_ACOS=\
	".\fdlibm.h"\
	

"$(INTDIR)\w_acosh.obj" : $(SOURCE) $(DEP_CPP_W_ACOS) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\w_asin.c
DEP_CPP_W_ASI=\
	".\fdlibm.h"\
	

"$(INTDIR)\w_asin.obj" : $(SOURCE) $(DEP_CPP_W_ASI) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\w_atan2.c
DEP_CPP_W_ATA=\
	".\fdlibm.h"\
	

"$(INTDIR)\w_atan2.obj" : $(SOURCE) $(DEP_CPP_W_ATA) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\w_atanh.c
DEP_CPP_W_ATAN=\
	".\fdlibm.h"\
	

"$(INTDIR)\w_atanh.obj" : $(SOURCE) $(DEP_CPP_W_ATAN) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\w_cosh.c
DEP_CPP_W_COS=\
	".\fdlibm.h"\
	

"$(INTDIR)\w_cosh.obj" : $(SOURCE) $(DEP_CPP_W_COS) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\w_exp.c
DEP_CPP_W_EXP=\
	".\fdlibm.h"\
	

"$(INTDIR)\w_exp.obj" : $(SOURCE) $(DEP_CPP_W_EXP) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\w_fmod.c
DEP_CPP_W_FMO=\
	".\fdlibm.h"\
	

"$(INTDIR)\w_fmod.obj" : $(SOURCE) $(DEP_CPP_W_FMO) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\w_gamma.c
DEP_CPP_W_GAM=\
	".\fdlibm.h"\
	

"$(INTDIR)\w_gamma.obj" : $(SOURCE) $(DEP_CPP_W_GAM) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\w_gamma_r.c
DEP_CPP_W_GAMM=\
	".\fdlibm.h"\
	

"$(INTDIR)\w_gamma_r.obj" : $(SOURCE) $(DEP_CPP_W_GAMM) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\w_hypot.c
DEP_CPP_W_HYP=\
	".\fdlibm.h"\
	

"$(INTDIR)\w_hypot.obj" : $(SOURCE) $(DEP_CPP_W_HYP) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\w_j0.c
DEP_CPP_W_J0_=\
	".\fdlibm.h"\
	

"$(INTDIR)\w_j0.obj" : $(SOURCE) $(DEP_CPP_W_J0_) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\w_j1.c
DEP_CPP_W_J1_=\
	".\fdlibm.h"\
	

"$(INTDIR)\w_j1.obj" : $(SOURCE) $(DEP_CPP_W_J1_) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\w_jn.c
DEP_CPP_W_JN_=\
	".\fdlibm.h"\
	

"$(INTDIR)\w_jn.obj" : $(SOURCE) $(DEP_CPP_W_JN_) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\w_lgamma.c
DEP_CPP_W_LGA=\
	".\fdlibm.h"\
	

"$(INTDIR)\w_lgamma.obj" : $(SOURCE) $(DEP_CPP_W_LGA) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\w_lgamma_r.c
DEP_CPP_W_LGAM=\
	".\fdlibm.h"\
	

"$(INTDIR)\w_lgamma_r.obj" : $(SOURCE) $(DEP_CPP_W_LGAM) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\w_log.c
DEP_CPP_W_LOG=\
	".\fdlibm.h"\
	

"$(INTDIR)\w_log.obj" : $(SOURCE) $(DEP_CPP_W_LOG) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\w_log10.c
DEP_CPP_W_LOG1=\
	".\fdlibm.h"\
	

"$(INTDIR)\w_log10.obj" : $(SOURCE) $(DEP_CPP_W_LOG1) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\w_pow.c
DEP_CPP_W_POW=\
	".\fdlibm.h"\
	

"$(INTDIR)\w_pow.obj" : $(SOURCE) $(DEP_CPP_W_POW) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\w_remainder.c
DEP_CPP_W_REM=\
	".\fdlibm.h"\
	

"$(INTDIR)\w_remainder.obj" : $(SOURCE) $(DEP_CPP_W_REM) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\w_scalb.c
DEP_CPP_W_SCA=\
	".\fdlibm.h"\
	

"$(INTDIR)\w_scalb.obj" : $(SOURCE) $(DEP_CPP_W_SCA) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\w_sinh.c
DEP_CPP_W_SIN=\
	".\fdlibm.h"\
	

"$(INTDIR)\w_sinh.obj" : $(SOURCE) $(DEP_CPP_W_SIN) "$(INTDIR)"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\e_acos.c
DEP_CPP_E_ACOS=\
	".\fdlibm.h"\
	

"$(INTDIR)\e_acos.obj" : $(SOURCE) $(DEP_CPP_E_ACOS) "$(INTDIR)"


# End Source File
# End Target
# End Project
################################################################################
