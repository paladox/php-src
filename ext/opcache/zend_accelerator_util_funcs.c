/*
   +----------------------------------------------------------------------+
   | Zend OPcache                                                         |
   +----------------------------------------------------------------------+
   | Copyright (c) 1998-2014 The PHP Group                                |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
   | Authors: Andi Gutmans <andi@zend.com>                                |
   |          Zeev Suraski <zeev@zend.com>                                |
   |          Stanislav Malyshev <stas@zend.com>                          |
   |          Dmitry Stogov <dmitry@zend.com>                             |
   +----------------------------------------------------------------------+
*/

#include "zend_API.h"
#include "zend_constants.h"
#include "zend_accelerator_util_funcs.h"
#include "zend_persist.h"
#include "zend_shared_alloc.h"

#define ZEND_PROTECTED_REFCOUNT	(1<<30)

static zend_uint zend_accel_refcount = ZEND_PROTECTED_REFCOUNT;

#if SIZEOF_SIZE_T <= SIZEOF_LONG
/* If sizeof(void*) == sizeof(ulong) we can use zend_hash index functions */
# define accel_xlat_set(old, new)	zend_hash_index_update_ptr(&ZCG(bind_hash), (ulong)(zend_uintptr_t)(old), (new))
# define accel_xlat_get(old)		zend_hash_index_find_ptr(&ZCG(bind_hash), (ulong)(zend_uintptr_t)(old))
#else
# define accel_xlat_set(old, new)	(zend_hash_str_add_ptr(&ZCG(bind_hash), (char*)&(old), sizeof(void*), (ulong)(zend_uintptr_t)(old), (void**)&(new))
# define accel_xlat_get(old, new)	((new) = zend_hash_str_find_ptr(&ZCG(bind_hash), (char*)&(old), sizeof(void*), (ulong)(zend_uintptr_t)(old), (void**)&(new)))
#endif

typedef int (*id_function_t)(void *, void *);
typedef void (*unique_copy_ctor_func_t)(void *pElement);

#if ZEND_EXTENSION_API_NO > PHP_5_3_X_API_NO
static const zend_uint uninitialized_bucket = {INVALID_IDX};
#endif

static int zend_prepare_function_for_execution(zend_op_array *op_array);
static void zend_hash_clone_zval(HashTable *ht, HashTable *source, int bind);
static zend_ast *zend_ast_clone(zend_ast *ast TSRMLS_DC);

static void zend_accel_destroy_zend_function(zval *zv)
{
	zend_function *function = Z_PTR_P(zv);
	TSRMLS_FETCH();

	if (function->type == ZEND_USER_FUNCTION) {
		if (function->op_array.static_variables) {

			FREE_HASHTABLE(function->op_array.static_variables);
			function->op_array.static_variables = NULL;
		}
	}

	destroy_zend_function(function TSRMLS_CC);
}

static void zend_accel_destroy_zend_class(zval *zv)
{
	zend_class_entry *ce = Z_PTR_P(zv);
	ce->function_table.pDestructor = zend_accel_destroy_zend_function;
	destroy_zend_class(zv);
}

zend_persistent_script* create_persistent_script(void)
{
	zend_persistent_script *persistent_script = (zend_persistent_script *) emalloc(sizeof(zend_persistent_script));
	memset(persistent_script, 0, sizeof(zend_persistent_script));

	zend_hash_init(&persistent_script->function_table, 128, NULL, (dtor_func_t) zend_accel_destroy_zend_function, 0);
	/* class_table is usually destroyed by free_persistent_script() that
	 * overrides destructor. ZEND_CLASS_DTOR may be used by standard
	 * PHP compiler
	 */
	zend_hash_init(&persistent_script->class_table, 16, NULL, ZEND_CLASS_DTOR, 0);

	return persistent_script;
}

static int compact_hash_table(HashTable *ht)
{
	uint i = 3;
	uint j;
	uint nSize;
	Bucket *d;
	Bucket *p;

	if (!ht->nNumOfElements || (ht->u.flags & HASH_FLAG_PACKED)) {
		/* Empty tables don't allocate space for Buckets */
		return 1;
	}

	if (ht->nNumOfElements >= 0x80000000) {
		/* prevent overflow */
		nSize = 0x80000000;
	} else {
		while ((1U << i) < ht->nNumOfElements) {
			i++;
		}
		nSize = 1 << i;
	}

	if (nSize >= ht->nTableSize) {
		/* Keep the size */
		return 1;
	}

	d = (Bucket *)pemalloc(nSize * (sizeof(Bucket) + sizeof(zend_uint)), ht->u.flags & HASH_FLAG_PERSISTENT);
	if (!d) {
		return 0;
	}

	for (i = 0, j = 0; i < ht->nNumUsed; i++) {
		p = ht->arData + i;
		if (Z_TYPE(p->val) != IS_UNDEF) {
			d[j++] = *p;
		}
	}
	ht->nNumUsed = j;

	pefree(ht->arData, ht->u.flags & HASH_FLAG_PERSISTENT);

	ht->arData = d;
	ht->arHash = (zend_uint *)(d + nSize);
	ht->nTableSize = nSize;
	ht->nTableMask = ht->nTableSize - 1;
	zend_hash_rehash(ht);
	
	return 1;
}

int compact_persistent_script(zend_persistent_script *persistent_script)
{
	return compact_hash_table(&persistent_script->function_table) &&
	       compact_hash_table(&persistent_script->class_table);
}

void free_persistent_script(zend_persistent_script *persistent_script, int destroy_elements)
{
	if (destroy_elements) {
		persistent_script->function_table.pDestructor = zend_accel_destroy_zend_function;
		persistent_script->class_table.pDestructor = zend_accel_destroy_zend_class;
	} else {
		persistent_script->function_table.pDestructor = NULL;
		persistent_script->class_table.pDestructor = NULL;
	}

	zend_hash_destroy(&persistent_script->function_table);
	zend_hash_destroy(&persistent_script->class_table);

	if (persistent_script->full_path) {
		efree(persistent_script->full_path);
	}

	efree(persistent_script);
}

static int is_not_internal_function(zval *zv)
{
	zend_function *function = Z_PTR_P(zv);
	return(function->type != ZEND_INTERNAL_FUNCTION);
}

void zend_accel_free_user_functions(HashTable *ht TSRMLS_DC)
{
	dtor_func_t orig_dtor = ht->pDestructor;

	ht->pDestructor = NULL;
	zend_hash_apply(ht, (apply_func_t) is_not_internal_function TSRMLS_CC);
	ht->pDestructor = orig_dtor;
}

static int move_user_function(zval *zv
#if ZEND_EXTENSION_API_NO >= PHP_5_3_X_API_NO
	TSRMLS_DC 
#endif
	, int num_args, va_list args, zend_hash_key *hash_key) 
{
	zend_function *function = Z_PTR_P(zv);
	HashTable *function_table = va_arg(args, HashTable *);
	(void)num_args; /* keep the compiler happy */
#if ZEND_EXTENSION_API_NO < PHP_5_3_X_API_NO
	TSRMLS_FETCH();
#endif 

	if (function->type == ZEND_USER_FUNCTION) {
		zend_hash_update_ptr(function_table, hash_key->key, function);
		return 1;
	} else {
		return 0;
	}
}

void zend_accel_move_user_functions(HashTable *src, HashTable *dst TSRMLS_DC)
{
	dtor_func_t orig_dtor = src->pDestructor;

	src->pDestructor = NULL;
#if ZEND_EXTENSION_API_NO < PHP_5_3_X_API_NO
	zend_hash_apply_with_arguments(src, (apply_func_args_t)move_user_function, 1, dst);
#else
	zend_hash_apply_with_arguments(src TSRMLS_CC, (apply_func_args_t)move_user_function, 1, dst);
#endif 
	src->pDestructor = orig_dtor;
}

static int copy_internal_function(zval *zv, HashTable *function_table TSRMLS_DC)
{
	zend_internal_function *function = Z_PTR_P(zv);
	if (function->type == ZEND_INTERNAL_FUNCTION) {
		zend_hash_update_mem(function_table, function->function_name, function, sizeof(zend_internal_function));
	}
	return 0;
}

void zend_accel_copy_internal_functions(TSRMLS_D)
{
	zend_hash_apply_with_argument(CG(function_table), (apply_func_arg_t)copy_internal_function, &ZCG(function_table) TSRMLS_CC);
	ZCG(internal_functions_count) = zend_hash_num_elements(&ZCG(function_table));
}

static void zend_destroy_property_info(zval *zv)
{
	zend_property_info *property_info = Z_PTR_P(zv);

	STR_RELEASE(property_info->name);
	if (property_info->doc_comment) {
		STR_RELEASE(property_info->doc_comment);
	}
	efree(property_info);
}

static inline zend_string *zend_clone_str(zend_string *str TSRMLS_DC)
{
	zend_string *ret;

	if (IS_INTERNED(str)) {		
		ret = str;
	} else if (STR_REFCOUNT(str) <= 1 || (ret = accel_xlat_get(str)) == NULL) {
		ret = STR_DUP(str, 0);
		GC_FLAGS(ret) = GC_FLAGS(str);
		if (STR_REFCOUNT(str) > 1) {
			accel_xlat_set(str, ret);
		}
	} else {
		STR_ADDREF(ret);
	}
	return ret;
}

static inline void zend_clone_zval(zval *src, int bind TSRMLS_DC)
{
	void *ptr;

	if (Z_IMMUTABLE_P(src)) {
		return;
	}

#if ZEND_EXTENSION_API_NO >= PHP_5_3_X_API_NO
	switch (Z_TYPE_P(src)) {
#else
	switch (Z_TYPE_P(src)) {
#endif
		case IS_STRING:
	    case IS_CONSTANT:
			Z_STR_P(src) = zend_clone_str(Z_STR_P(src) TSRMLS_CC);
			break;
		case IS_ARRAY:
#if ZEND_EXTENSION_API_NO <= PHP_5_5_API_NO
	    case IS_CONSTANT_ARRAY:
#endif
			if (Z_ARR_P(src) != &EG(symbol_table)) {
		    	if (bind && Z_REFCOUNT_P(src) > 1 && (ptr = accel_xlat_get(Z_ARR_P(src))) != NULL) {
		    		Z_ARR_P(src) = ptr;
				} else {
					zend_array *old = Z_ARR_P(src);

					Z_ARR_P(src) = emalloc(sizeof(zend_array));
					Z_ARR_P(src)->gc = old->gc;
			    	if (bind && Z_REFCOUNT_P(src) > 1) {
						accel_xlat_set(old, Z_ARR_P(src));
					}
					zend_hash_clone_zval(Z_ARRVAL_P(src), &old->ht, 0);
				}
			}
			break;
	    case IS_REFERENCE:
	    	if (bind && Z_REFCOUNT_P(src) > 1 && (ptr = accel_xlat_get(Z_REF_P(src))) != NULL) {
	    		Z_REF_P(src) = ptr;
			} else {
				zend_reference *old = Z_REF_P(src);
				ZVAL_NEW_REF(src, &old->val);
				Z_REF_P(src)->gc = old->gc;
		    	if (bind && Z_REFCOUNT_P(src) > 1) {
					accel_xlat_set(old, Z_REF_P(src));
				}
				zend_clone_zval(Z_REFVAL_P(src), bind TSRMLS_CC);
			}
	    	break;
	    case IS_CONSTANT_AST:
	    	if (bind && Z_REFCOUNT_P(src) > 1 && (ptr = accel_xlat_get(Z_AST_P(src))) != NULL) {
	    		Z_AST_P(src) = ptr;
			} else {
				zend_ast_ref *old = Z_AST_P(src);

		    	ZVAL_NEW_AST(src, old->ast);
				Z_AST_P(src)->gc = old->gc;
		    	if (bind && Z_REFCOUNT_P(src) > 1) {
					accel_xlat_set(old, Z_AST_P(src));
				}
		    	Z_ASTVAL_P(src) = zend_ast_clone(Z_ASTVAL_P(src) TSRMLS_CC);
			}
	    	break;
	}
}

#if ZEND_EXTENSION_API_NO > PHP_5_5_X_API_NO
static zend_ast *zend_ast_clone(zend_ast *ast TSRMLS_DC)
{
	int i;
	zend_ast *node;

	if (ast->kind == ZEND_CONST) {
		node = emalloc(sizeof(zend_ast) + sizeof(zval));
		node->kind = ZEND_CONST;
		node->children = 0;
		ZVAL_COPY_VALUE(&node->u.val, &ast->u.val);
		zend_clone_zval(&node->u.val, 0 TSRMLS_CC);
	} else {
		node = emalloc(sizeof(zend_ast) + sizeof(zend_ast*) * (ast->children - 1));
		node->kind = ast->kind;
		node->children = ast->children;
		for (i = 0; i < ast->children; i++) {
			if ((&ast->u.child)[i]) {
				(&node->u.child)[i] = zend_ast_clone((&ast->u.child)[i] TSRMLS_CC);
			} else {
				(&node->u.child)[i] = NULL;
			}
		}
	}
	return node;
}
#endif

static void zend_hash_clone_zval(HashTable *ht, HashTable *source, int bind)
{
	uint idx;
	Bucket *p, *q, *r;
	ulong nIndex;
	TSRMLS_FETCH();

	ht->nTableSize = source->nTableSize;
	ht->nTableMask = source->nTableMask;
	ht->nNumUsed = 0;
	ht->nNumOfElements = source->nNumOfElements;
	ht->nNextFreeElement = source->nNextFreeElement;
	ht->pDestructor = ZVAL_PTR_DTOR;
	ht->u.flags = HASH_FLAG_APPLY_PROTECTION;
	ht->arData = NULL;
	ht->arHash = NULL;
	ht->nInternalPointer = source->nNumOfElements ? 0 : INVALID_IDX;

#if ZEND_EXTENSION_API_NO > PHP_5_3_X_API_NO
	if (!ht->nTableMask) {
		ht->arHash = (zend_uint*)&uninitialized_bucket;
		return;
	}
#endif

	if (source->u.flags & HASH_FLAG_PACKED) {
		ht->u.flags |= HASH_FLAG_PACKED;
		ht->arData = (Bucket *) emalloc(ht->nTableSize * sizeof(Bucket));
		ht->arHash = (zend_uint*)&uninitialized_bucket;
	
		for (idx = 0; idx < source->nNumUsed; idx++) {
			p = source->arData + idx;
			if (Z_TYPE(p->val) == IS_UNDEF) continue;
			nIndex = p->h & ht->nTableMask;

			r = ht->arData + ht->nNumUsed;
			q = ht->arData + p->h;
			while (r != q) {
				ZVAL_UNDEF(&r->val);
				r++;
			}
			ht->nNumUsed = p->h + 1;

			/* Initialize key */
			q->h = p->h;
			q->key = NULL;

			/* Copy data */
			ZVAL_COPY_VALUE(&q->val, &p->val);
			zend_clone_zval(&q->val, bind TSRMLS_CC);
		}
	} else {
		ht->arData = (Bucket *) emalloc(ht->nTableSize * (sizeof(Bucket) + sizeof(zend_uint)));
		ht->arHash = (zend_uint*)(ht->arData + ht->nTableSize);
		memset(ht->arHash, INVALID_IDX, sizeof(zend_uint) * ht->nTableSize);
	
		for (idx = 0; idx < source->nNumUsed; idx++) {
			p = source->arData + idx;
			if (Z_TYPE(p->val) == IS_UNDEF) continue;
			nIndex = p->h & ht->nTableMask;

			/* Insert into hash collision list */
			q = ht->arData + ht->nNumUsed;
			Z_NEXT(q->val) = ht->arHash[nIndex];
			ht->arHash[nIndex] = ht->nNumUsed++;

			/* Initialize key */
			q->h = p->h;
			if (!p->key) {
				q->key = NULL;
			} else {
				q->key = zend_clone_str(p->key TSRMLS_CC);
			}

			/* Copy data */
			ZVAL_COPY_VALUE(&q->val, &p->val);
			zend_clone_zval(&q->val, bind TSRMLS_CC);
		}
	}
}

static void zend_hash_clone_methods(HashTable *ht, HashTable *source, zend_class_entry *old_ce, zend_class_entry *ce TSRMLS_DC)
{
	uint idx;
	Bucket *p, *q;
	ulong nIndex;
	zend_class_entry *new_ce;
	zend_function *new_prototype;
	zend_op_array *new_entry;

	ht->nTableSize = source->nTableSize;
	ht->nTableMask = source->nTableMask;
	ht->nNumUsed = 0;
	ht->nNumOfElements = source->nNumOfElements;
	ht->nNextFreeElement = source->nNextFreeElement;
	ht->pDestructor = ZEND_FUNCTION_DTOR;
	ht->u.flags = HASH_FLAG_APPLY_PROTECTION;
	ht->nInternalPointer = source->nNumOfElements ? 0 : INVALID_IDX;

#if ZEND_EXTENSION_API_NO > PHP_5_3_X_API_NO
	if (!ht->nTableMask) {
		ht->arHash = (zend_uint*)&uninitialized_bucket;
		return;
	}
#endif

	ZEND_ASSERT(!(source->u.flags & HASH_FLAG_PACKED));
	ht->arData = (Bucket *) emalloc(ht->nTableSize * (sizeof(Bucket) + sizeof(zend_uint)));
	ht->arHash = (zend_uint *)(ht->arData + ht->nTableSize);
	memset(ht->arHash, INVALID_IDX, sizeof(zend_uint) * ht->nTableSize);

	for (idx = 0; idx < source->nNumUsed; idx++) {		
		p = source->arData + idx;
		if (Z_TYPE(p->val) == IS_UNDEF) continue;

		nIndex = p->h & ht->nTableMask;

		/* Insert into hash collision list */
		q = ht->arData + ht->nNumUsed;
		Z_NEXT(q->val) = ht->arHash[nIndex];
		ht->arHash[nIndex] = ht->nNumUsed++;

		/* Initialize key */
		q->h = p->h;
		ZEND_ASSERT(p->key != NULL);
		q->key = zend_clone_str(p->key TSRMLS_CC);

		/* Copy data */
		ZVAL_PTR(&q->val, (void *) emalloc(sizeof(zend_op_array)));
		new_entry = (zend_op_array*)Z_PTR(q->val);
		*new_entry = *(zend_op_array*)Z_PTR(p->val);

		/* Copy constructor */
		/* we use refcount to show that op_array is referenced from several places */
		if (new_entry->refcount != NULL) {
			accel_xlat_set(Z_PTR(p->val), new_entry);
		}

		zend_prepare_function_for_execution(new_entry);

		if (old_ce == new_entry->scope) {
			new_entry->scope = ce;
		} else {
			if ((new_ce = accel_xlat_get(new_entry->scope)) != NULL) {
				new_entry->scope = new_ce;
			} else {
				zend_error(E_ERROR, ACCELERATOR_PRODUCT_NAME " class loading error, class %s, function %s", ce->name->val, new_entry->function_name->val);
			}
		}

		/* update prototype */
		if (new_entry->prototype) {
			if ((new_prototype = accel_xlat_get(new_entry->prototype)) != NULL) {
				new_entry->prototype = new_prototype;
			} else {
				zend_error(E_ERROR, ACCELERATOR_PRODUCT_NAME " class loading error, class %s, function %s", ce->name->val, new_entry->function_name->val);
			}
		}
	}
}

static void zend_hash_clone_prop_info(HashTable *ht, HashTable *source, zend_class_entry *old_ce, zend_class_entry *ce TSRMLS_DC)
{
	uint idx;
	Bucket *p, *q;
	ulong nIndex;
	zend_class_entry *new_ce;
	zend_property_info *prop_info;

	ht->nTableSize = source->nTableSize;
	ht->nTableMask = source->nTableMask;
	ht->nNumUsed = 0;
	ht->nNumOfElements = source->nNumOfElements;
	ht->nNextFreeElement = source->nNextFreeElement;
	ht->pDestructor = zend_destroy_property_info;
	ht->u.flags = HASH_FLAG_APPLY_PROTECTION;
	ht->nInternalPointer = source->nNumOfElements ? 0 : INVALID_IDX;

#if ZEND_EXTENSION_API_NO > PHP_5_3_X_API_NO
	if (!ht->nTableMask) {
		ht->arHash = (zend_uint*)&uninitialized_bucket;
		return;
	}
#endif

	ZEND_ASSERT(!(source->u.flags & HASH_FLAG_PACKED));
	ht->arData = (Bucket *) emalloc(ht->nTableSize * (sizeof(Bucket) + sizeof(zend_uint)));
	ht->arHash = (zend_uint*)(ht->arData + ht->nTableSize);
	memset(ht->arHash, INVALID_IDX, sizeof(zend_uint) * ht->nTableSize);

	for (idx = 0; idx < source->nNumUsed; idx++) {		
		p = source->arData + idx;
		if (Z_TYPE(p->val) == IS_UNDEF) continue;

		nIndex = p->h & ht->nTableMask;

		/* Insert into hash collision list */
		q = ht->arData + ht->nNumUsed;
		Z_NEXT(q->val) = ht->arHash[nIndex];
		ht->arHash[nIndex] = ht->nNumUsed++;

		/* Initialize key */
		q->h = p->h;
		ZEND_ASSERT(p->key != NULL);
		q->key = zend_clone_str(p->key TSRMLS_CC);

		/* Copy data */
		ZVAL_PTR(&q->val, (void *) emalloc(sizeof(zend_property_info)));
		prop_info = Z_PTR(q->val);
		*prop_info = *(zend_property_info*)Z_PTR(p->val);

		/* Copy constructor */
		prop_info->name = zend_clone_str(prop_info->name TSRMLS_CC);
		if (prop_info->doc_comment) {
			if (ZCG(accel_directives).load_comments) {
				prop_info->doc_comment = STR_DUP(prop_info->doc_comment, 0);
			} else {
				prop_info->doc_comment = NULL;
			}
		}
		if (prop_info->ce == old_ce) {
			prop_info->ce = ce;
		} else if ((new_ce = accel_xlat_get(prop_info->ce)) != NULL) {
			prop_info->ce = new_ce;
		} else {
			zend_error(E_ERROR, ACCELERATOR_PRODUCT_NAME" class loading error, class %s, property %s", ce->name->val, prop_info->name->val);
		}
	}
}

/* protects reference count, creates copy of statics */
static int zend_prepare_function_for_execution(zend_op_array *op_array)
{
	HashTable *shared_statics = op_array->static_variables;

	/* protect reference count */
	op_array->refcount = &zend_accel_refcount;
	(*op_array->refcount) = ZEND_PROTECTED_REFCOUNT;

	/* copy statics */
	if (shared_statics) {
		ALLOC_HASHTABLE(op_array->static_variables);
		zend_hash_clone_zval(op_array->static_variables, shared_statics, 0);
	}

	return 0;
}

#define zend_update_inherited_handler(handler) \
{ \
	if (ce->handler != NULL) { \
		if ((new_func = accel_xlat_get(ce->handler)) != NULL) { \
			ce->handler = new_func; \
		} else { \
			zend_error(E_ERROR, ACCELERATOR_PRODUCT_NAME " class loading error, class %s", ce->name->val); \
		} \
	} \
}

/* Protects class' refcount, copies default properties, functions and class name */
static void zend_class_copy_ctor(zend_class_entry **pce)
{
	zend_class_entry *ce = *pce;
	zend_class_entry *old_ce = ce;
	zend_class_entry *new_ce;
	zend_function *new_func;
	TSRMLS_FETCH();

	*pce = ce = emalloc(sizeof(zend_class_entry));
	*ce = *old_ce;
	ce->refcount = 1;

	if (old_ce->refcount != 1) {
		/* this class is not used as a parent for any other classes */
		accel_xlat_set(old_ce, ce);
	}

#if ZEND_EXTENSION_API_NO > PHP_5_3_X_API_NO
	if (old_ce->default_properties_table) {
		int i;

		ce->default_properties_table = emalloc(sizeof(zval) * old_ce->default_properties_count);
		for (i = 0; i < old_ce->default_properties_count; i++) {
			ZVAL_COPY_VALUE(&ce->default_properties_table[i], &old_ce->default_properties_table[i]);
			zend_clone_zval(&ce->default_properties_table[i], 1 TSRMLS_CC);
		}
	}
#else
	zend_hash_clone_zval(&ce->default_properties, &old_ce->default_properties, 0);
#endif

	zend_hash_clone_methods(&ce->function_table, &old_ce->function_table, old_ce, ce TSRMLS_CC);

	/* static members */
#if ZEND_EXTENSION_API_NO > PHP_5_3_X_API_NO
	if (old_ce->default_static_members_table) {
		int i;

		ce->default_static_members_table = emalloc(sizeof(zval) * old_ce->default_static_members_count);
		for (i = 0; i < old_ce->default_static_members_count; i++) {
			ZVAL_COPY_VALUE(&ce->default_static_members_table[i], &old_ce->default_static_members_table[i]);
			zend_clone_zval(&ce->default_static_members_table[i], 1 TSRMLS_CC);
		}
	}
	ce->static_members_table = ce->default_static_members_table;
#else
	zend_hash_clone_zval(&ce->default_static_members, &old_ce->default_static_members, 1);
	ce->static_members = &ce->default_static_members;
#endif

	/* properties_info */
	zend_hash_clone_prop_info(&ce->properties_info, &old_ce->properties_info, old_ce, ce TSRMLS_CC);

	/* constants table */
	zend_hash_clone_zval(&ce->constants_table, &old_ce->constants_table, 1);

	ce->name = zend_clone_str(ce->name TSRMLS_CC);

	/* interfaces aren't really implemented, so we create a new table */
	if (ce->num_interfaces) {
		ce->interfaces = emalloc(sizeof(zend_class_entry *) * ce->num_interfaces);
		memset(ce->interfaces, 0, sizeof(zend_class_entry *) * ce->num_interfaces);
	} else {
		ce->interfaces = NULL;
	}
	if (ZEND_CE_DOC_COMMENT(ce)) {
		if (ZCG(accel_directives).load_comments) {
			ZEND_CE_DOC_COMMENT(ce) = STR_DUP(ZEND_CE_DOC_COMMENT(ce), 0);
		} else {
			ZEND_CE_DOC_COMMENT(ce) =  NULL;
		}
	}

	if (ce->parent) {
		if ((new_ce = accel_xlat_get(ce->parent)) != NULL) {
			ce->parent = new_ce;
		} else {
			zend_error(E_ERROR, ACCELERATOR_PRODUCT_NAME" class loading error, class %s", ce->name->val);
		}
	}

	zend_update_inherited_handler(constructor);
	zend_update_inherited_handler(destructor);
	zend_update_inherited_handler(clone);
	zend_update_inherited_handler(__get);
	zend_update_inherited_handler(__set);
	zend_update_inherited_handler(__call);
/* 5.1 stuff */
	zend_update_inherited_handler(serialize_func);
	zend_update_inherited_handler(unserialize_func);
	zend_update_inherited_handler(__isset);
	zend_update_inherited_handler(__unset);
/* 5.2 stuff */
	zend_update_inherited_handler(__tostring);

#if ZEND_EXTENSION_API_NO >= PHP_5_3_X_API_NO
/* 5.3 stuff */
	zend_update_inherited_handler(__callstatic);
#endif
	zend_update_inherited_handler(__debugInfo);

#if ZEND_EXTENSION_API_NO > PHP_5_3_X_API_NO
/* 5.4 traits */
	if (ce->trait_aliases) {
		zend_trait_alias **trait_aliases;
		int i = 0;

		while (ce->trait_aliases[i]) {
			i++;
		}
		trait_aliases = emalloc(sizeof(zend_trait_alias*) * (i + 1));
		i = 0;
		while (ce->trait_aliases[i]) {
			trait_aliases[i] = emalloc(sizeof(zend_trait_alias));
			memcpy(trait_aliases[i], ce->trait_aliases[i], sizeof(zend_trait_alias));
			trait_aliases[i]->trait_method = emalloc(sizeof(zend_trait_method_reference));
			memcpy(trait_aliases[i]->trait_method, ce->trait_aliases[i]->trait_method, sizeof(zend_trait_method_reference));
			if (trait_aliases[i]->trait_method) {
				if (trait_aliases[i]->trait_method->method_name) {
					trait_aliases[i]->trait_method->method_name =
						zend_clone_str(trait_aliases[i]->trait_method->method_name TSRMLS_CC);
				}
				if (trait_aliases[i]->trait_method->class_name) {
					trait_aliases[i]->trait_method->class_name =
						zend_clone_str(trait_aliases[i]->trait_method->class_name TSRMLS_CC);
				}
			}

			if (trait_aliases[i]->alias) {
				trait_aliases[i]->alias =
					zend_clone_str(trait_aliases[i]->alias TSRMLS_CC);
			}
			i++;
		}
		trait_aliases[i] = NULL;
		ce->trait_aliases = trait_aliases;
	}

	if (ce->trait_precedences) {
		zend_trait_precedence **trait_precedences;
		int i = 0;

		while (ce->trait_precedences[i]) {
			i++;
		}
		trait_precedences = emalloc(sizeof(zend_trait_precedence*) * (i + 1));
		i = 0;
		while (ce->trait_precedences[i]) {
			trait_precedences[i] = emalloc(sizeof(zend_trait_precedence));
			memcpy(trait_precedences[i], ce->trait_precedences[i], sizeof(zend_trait_precedence));
			trait_precedences[i]->trait_method = emalloc(sizeof(zend_trait_method_reference));
			memcpy(trait_precedences[i]->trait_method, ce->trait_precedences[i]->trait_method, sizeof(zend_trait_method_reference));

			trait_precedences[i]->trait_method->method_name =
				zend_clone_str(trait_precedences[i]->trait_method->method_name TSRMLS_CC);
			trait_precedences[i]->trait_method->class_name =
				zend_clone_str(trait_precedences[i]->trait_method->class_name TSRMLS_CC);

			if (trait_precedences[i]->exclude_from_classes) {
				zend_string **exclude_from_classes;
				int j = 0;

				while (trait_precedences[i]->exclude_from_classes[j].class_name) {
					j++;
				}
				exclude_from_classes = emalloc(sizeof(zend_string*) * (j + 1));
				j = 0;
				while (trait_precedences[i]->exclude_from_classes[j].class_name) {
					exclude_from_classes[j] =
						zend_clone_str(trait_precedences[i]->exclude_from_classes[j].class_name TSRMLS_CC);
					j++;
				}
				exclude_from_classes[j] = NULL;
				trait_precedences[i]->exclude_from_classes = (void*)exclude_from_classes;
			}
			i++;
		}
		trait_precedences[i] = NULL;
		ce->trait_precedences = trait_precedences;
	}
#endif
}

static void zend_accel_function_hash_copy(HashTable *target, HashTable *source, unique_copy_ctor_func_t pCopyConstructor TSRMLS_DC)
{
	zend_function *function1, *function2;
	uint idx;
	Bucket *p;
	zval *t;

	for (idx = 0; idx < source->nNumUsed; idx++) {		
		p = source->arData + idx;
		if (Z_TYPE(p->val) == IS_UNDEF) continue;
		if (p->key) {
			t = zend_hash_add(target, p->key, &p->val);
			if (UNEXPECTED(t == NULL)) {
				if (p->key->len > 0 && p->key->val[0] == 0) {
					/* Mangled key */
#if ZEND_EXTENSION_API_NO >= PHP_5_3_X_API_NO
					if (((zend_function*)Z_PTR(p->val))->common.fn_flags & ZEND_ACC_CLOSURE) {
						/* update closure */
						t = zend_hash_update(target, p->key, &p->val);
					} else {
						/* ignore and wait for runtime */
						continue;
					} 
#else
					/* ignore and wait for runtime */
					continue;
#endif
				} else {
					t = zend_hash_find(target, p->key);
					goto failure;
				}
			}
		} else {
		    t = zend_hash_index_add(target, p->h, &p->val);
			if (UNEXPECTED(t == NULL)) {
				t = zend_hash_index_find(target, p->h);				
				goto failure;
			}
		}
		if (pCopyConstructor) {
			Z_PTR_P(t) = emalloc(sizeof(zend_function));
			memcpy(Z_PTR_P(t), Z_PTR(p->val), sizeof(zend_function));
			pCopyConstructor(Z_PTR_P(t));
		}
	}
	target->nInternalPointer = target->nNumOfElements ? 0 : INVALID_IDX;
	return;

failure:
	function1 = Z_PTR(p->val);
	function2 = Z_PTR_P(t);
	CG(in_compilation) = 1;
	zend_set_compiled_filename(function1->op_array.filename TSRMLS_CC);
	CG(zend_lineno) = function1->op_array.opcodes[0].lineno;
	if (function2->type == ZEND_USER_FUNCTION
		&& function2->op_array.last > 0) {
		zend_error(E_ERROR, "Cannot redeclare %s() (previously declared in %s:%d)",
				   function1->common.function_name->val,
				   function2->op_array.filename->val,
				   (int)function2->op_array.opcodes[0].lineno);
	} else {
		zend_error(E_ERROR, "Cannot redeclare %s()", function1->common.function_name->val);
	}
}

static void zend_accel_class_hash_copy(HashTable *target, HashTable *source, unique_copy_ctor_func_t pCopyConstructor TSRMLS_DC)
{
	zend_class_entry *ce1;
	uint idx;
	Bucket *p;
	zval *t;

	for (idx = 0; idx < source->nNumUsed; idx++) {		
		p = source->arData + idx;
		if (Z_TYPE(p->val) == IS_UNDEF) continue;
		if (p->key) {
			t = zend_hash_add(target, p->key, &p->val);
			if (UNEXPECTED(t == NULL)) {
				if (p->key->len > 0 && p->key->val[0] == 0) {
					/* Mangled key - ignore and wait for runtime */
					continue;
				} else if (!ZCG(accel_directives).ignore_dups) {
					t = zend_hash_find(target, p->key);
					goto failure;
				}
			}
		} else {
			t = zend_hash_index_add(target, p->h, &p->val);
			if (UNEXPECTED(t == NULL)) {
				if (!ZCG(accel_directives).ignore_dups) {
					t = zend_hash_index_find(target,p->h);
					goto failure;
				}
			}
		}
		if (pCopyConstructor) {
			pCopyConstructor(&Z_PTR_P(t));
		}
	}
	target->nInternalPointer = target->nNumOfElements ? 0 : INVALID_IDX;
	return;

failure:
	ce1 = Z_PTR(p->val);
	CG(in_compilation) = 1;
#if ZEND_EXTENSION_API_NO > PHP_5_3_X_API_NO
	zend_set_compiled_filename(ce1->info.user.filename TSRMLS_CC);
	CG(zend_lineno) = ce1->info.user.line_start;
#else
	zend_set_compiled_filename(ce1->filename TSRMLS_CC);
	CG(zend_lineno) = ce1->line_start;
#endif
	zend_error(E_ERROR, "Cannot redeclare class %s", ce1->name->val);
}

#if ZEND_EXTENSION_API_NO < PHP_5_3_X_API_NO
static void zend_do_delayed_early_binding(zend_op_array *op_array, zend_uint early_binding TSRMLS_DC)
{
	zend_uint opline_num = early_binding;

	if ((int)opline_num != -1) {
		zend_bool orig_in_compilation = CG(in_compilation);
		char *orig_compiled_filename = zend_set_compiled_filename(op_array->filename TSRMLS_CC);
		zend_class_entry **pce;

		CG(in_compilation) = 1;
		while ((int)opline_num != -1) {
			if (zend_lookup_class(Z_STRVAL(op_array->opcodes[opline_num - 1].op2.u.constant), Z_STRLEN(op_array->opcodes[opline_num - 1].op2.u.constant), &pce TSRMLS_CC) == SUCCESS) {
				do_bind_inherited_class(&op_array->opcodes[opline_num], EG(class_table), *pce, 1 TSRMLS_CC);
			}
			opline_num = op_array->opcodes[opline_num].result.u.opline_num;
		}
		zend_restore_compiled_filename(orig_compiled_filename TSRMLS_CC);
		CG(in_compilation) = orig_in_compilation;
	}
}
#endif

zend_op_array* zend_accel_load_script(zend_persistent_script *persistent_script, int from_shared_memory TSRMLS_DC)
{
	zend_op_array *op_array;

	op_array = (zend_op_array *) emalloc(sizeof(zend_op_array));
	*op_array = persistent_script->main_op_array;

	if (from_shared_memory) {
		zend_hash_init(&ZCG(bind_hash), 10, NULL, NULL, 0);

		/* Copy all the necessary stuff from shared memory to regular memory, and protect the shared script */
		if (zend_hash_num_elements(&persistent_script->class_table) > 0) {
			zend_accel_class_hash_copy(CG(class_table), &persistent_script->class_table, (unique_copy_ctor_func_t) zend_class_copy_ctor TSRMLS_CC);
		}
		/* we must first to copy all classes and then prepare functions, since functions may try to bind
		   classes - which depend on pre-bind class entries existent in the class table */
		if (zend_hash_num_elements(&persistent_script->function_table) > 0) {
			zend_accel_function_hash_copy(CG(function_table), &persistent_script->function_table, (unique_copy_ctor_func_t)zend_prepare_function_for_execution TSRMLS_CC);
		}

		zend_prepare_function_for_execution(op_array);

		/* Register __COMPILER_HALT_OFFSET__ constant */
		if (persistent_script->compiler_halt_offset != 0 &&
		    persistent_script->full_path) {
			zend_string *name;
			char haltoff[] = "__COMPILER_HALT_OFFSET__";

			name = zend_mangle_property_name(haltoff, sizeof(haltoff) - 1, persistent_script->full_path->val, persistent_script->full_path->len, 0);
			if (!zend_hash_exists(EG(zend_constants), name)) {
				zend_register_long_constant(name->val, name->len, persistent_script->compiler_halt_offset, CONST_CS, 0 TSRMLS_CC);
			}
			STR_RELEASE(name);
		}

		zend_hash_destroy(&ZCG(bind_hash));

#if ZEND_EXTENSION_API_NO < PHP_5_3_X_API_NO
		if ((int)persistent_script->early_binding != -1) {
			zend_do_delayed_early_binding(op_array, persistent_script->early_binding TSRMLS_CC);
		}
#endif

	} else /* if (!from_shared_memory) */ {
		if (zend_hash_num_elements(&persistent_script->function_table) > 0) {
			zend_accel_function_hash_copy(CG(function_table), &persistent_script->function_table, NULL TSRMLS_CC);
		}
		if (zend_hash_num_elements(&persistent_script->class_table) > 0) {
			zend_accel_class_hash_copy(CG(class_table), &persistent_script->class_table, NULL TSRMLS_CC);
		}
		free_persistent_script(persistent_script, 0); /* free only hashes */
	}

#if ZEND_EXTENSION_API_NO >= PHP_5_3_X_API_NO
	if (op_array->early_binding != (zend_uint)-1) {
		zend_string *orig_compiled_filename = CG(compiled_filename);
		CG(compiled_filename) = persistent_script->full_path;
		zend_do_delayed_early_binding(op_array TSRMLS_CC);
		CG(compiled_filename) = orig_compiled_filename;
	}
#endif

	return op_array;
}

/*
 * zend_adler32() is based on zlib implementation
 * Computes the Adler-32 checksum of a data stream
 *
 * Copyright (C) 1995-2005 Mark Adler
 * For conditions of distribution and use, see copyright notice in zlib.h
 *
 * Copyright (C) 1995-2005 Jean-loup Gailly and Mark Adler
 *
 *  This software is provided 'as-is', without any express or implied
 *  warranty.  In no event will the authors be held liable for any damages
 *  arising from the use of this software.
 *
 *  Permission is granted to anyone to use this software for any purpose,
 *  including commercial applications, and to alter it and redistribute it
 *  freely, subject to the following restrictions:
 *
 *  1. The origin of this software must not be misrepresented; you must not
 *     claim that you wrote the original software. If you use this software
 *     in a product, an acknowledgment in the product documentation would be
 *     appreciated but is not required.
 *  2. Altered source versions must be plainly marked as such, and must not be
 *     misrepresented as being the original software.
 *  3. This notice may not be removed or altered from any source distribution.
 *
 */

#define ADLER32_BASE 65521 /* largest prime smaller than 65536 */
#define ADLER32_NMAX 5552
/* NMAX is the largest n such that 255n(n+1)/2 + (n+1)(BASE-1) <= 2^32-1 */

#define ADLER32_DO1(buf)        {s1 += *(buf); s2 += s1;}
#define ADLER32_DO2(buf, i)     ADLER32_DO1(buf + i); ADLER32_DO1(buf + i + 1);
#define ADLER32_DO4(buf, i)     ADLER32_DO2(buf, i); ADLER32_DO2(buf, i + 2);
#define ADLER32_DO8(buf, i)     ADLER32_DO4(buf, i); ADLER32_DO4(buf, i + 4);
#define ADLER32_DO16(buf)       ADLER32_DO8(buf, 0); ADLER32_DO8(buf, 8);

unsigned int zend_adler32(unsigned int checksum, signed char *buf, uint len)
{
	unsigned int s1 = checksum & 0xffff;
	unsigned int s2 = (checksum >> 16) & 0xffff;
	signed char *end;

	while (len >= ADLER32_NMAX) {
		len -= ADLER32_NMAX;
		end = buf + ADLER32_NMAX;
		do {
			ADLER32_DO16(buf);
			buf += 16;
		} while (buf != end);
		s1 %= ADLER32_BASE;
		s2 %= ADLER32_BASE;
	}

	if (len) {
		if (len >= 16) {
			end = buf + (len & 0xfff0);
			len &= 0xf;
			do {
				ADLER32_DO16(buf);
				buf += 16;
			} while (buf != end);
		}
		if (len) {
			end = buf + len;
			do {
				ADLER32_DO1(buf);
				buf++;
			} while (buf != end);
		}
		s1 %= ADLER32_BASE;
		s2 %= ADLER32_BASE;
	}

	return (s2 << 16) | s1;
}