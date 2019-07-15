# man pages that go into section 3:
man3_MANS = libunwind.man libunwind-dynamic.man libunwind-ia64.man	\
	libunwind-ptrace.man libunwind-setjmp.man			\
	unw_apply_reg_state.man						\
	unw_backtrace.man						\
	unw_flush_cache.man						\
	unw_get_accessors.man						\
	unw_get_proc_info.man						\
	unw_get_proc_info_by_ip.man					\
	unw_get_proc_name.man						\
	unw_get_fpreg.man						\
	unw_get_reg.man							\
	unw_getcontext.man						\
	unw_init_local.man unw_init_remote.man				\
	unw_init_local2.man						\
	unw_is_fpreg.man						\
	unw_is_signal_frame.man						\
	unw_create_addr_space.man					\
	unw_destroy_addr_space.man					\
	unw_regname.man unw_resume.man					\
	unw_reg_states_iterate.man					\
	unw_set_caching_policy.man					\
	unw_set_cache_size.man						\
	unw_set_fpreg.man						\
	unw_set_reg.man							\
	unw_step.man							\
	unw_strerror.man						\
	_U_dyn_register.man						\
	_U_dyn_cancel.man

EXTRA_DIST = NOTES libunwind.trans					\
	libunwind.tex libunwind-dynamic.tex libunwind-ia64.tex		\
	libunwind-ptrace.tex libunwind-setjmp.tex			\
	unw_apply_reg_state.tex						\
	unw_backtrace.tex						\
	unw_flush_cache.tex						\
	unw_get_accessors.tex						\
	unw_get_proc_info.tex						\
	unw_get_proc_info_by_ip.tex					\
	unw_get_proc_name.tex						\
	unw_get_fpreg.tex						\
	unw_get_reg.tex							\
	unw_getcontext.tex						\
	unw_init_local.tex unw_init_remote.tex				\
	unw_is_fpreg.tex						\
	unw_is_signal_frame.tex						\
	unw_create_addr_space.tex unw_destroy_addr_space.tex		\
	unw_regname.tex unw_resume.tex unw_set_caching_policy.tex	\
	unw_reg_states_iterate.tex					\
	unw_set_cache_size.tex						\
	unw_set_fpreg.tex						\
	unw_set_reg.tex							\
	unw_step.tex							\
	unw_strerror.tex						\
	_U_dyn_register.tex						\
	_U_dyn_cancel.tex						\
	$(man3_MANS)

L2M	= latex2man
L2P	= pdflatex
L2M_CMD	= $(L2M) -t $(srcdir)/libunwind.trans
L2H_CMD	= $(L2M) -H -t $(srcdir)/libunwind.trans

.tex.man:
	$(L2M_CMD) $< $@
	-cp $@ $(srcdir)/$@

html:
	for n in $(man3_MANS); do					\
		page=`basename $$n .man`;				\
		$(L2H_CMD) $(srcdir)/$$page.tex "$$page(3).raw";	\
	done

pdf:
	for n in $(man3_MANS); do					\
		page=`basename $$n .man`;				\
		$(L2P) $(srcdir)/$$page.tex "$$page(3).pdf";		\
	done

MAINTAINERCLEANFILES = Makefile.in
