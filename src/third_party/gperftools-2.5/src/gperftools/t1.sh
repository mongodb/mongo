
TCMALLOC_H=tcmalloc.h
TCMALLOC_H_TMP=tcmalloc.h.bak
for line_number in $(grep -n "@ac_cv_have_struct_mallinfo@" tcmalloc.h.in | cut -d: -f1) ; do
    sed "${line_number}s/.*/#ifdef HAVE_STRUCT_MALLINFO/" < $TCMALLOC_H > $TCMALLOC_H_TMP
    cp $TCMALLOC_H_TMP $TCMALLOC_H
done

