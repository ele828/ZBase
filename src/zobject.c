#include "zobject.h"
#include "debug.h"
#include "mem.h"
#include "rb_tree.h"
#include "container.h"
#include "opt.h"
#include "bits.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#define to_class(node) container_of(node, struct ZObjClass, rb_node)

static RBTree class_tree;
//static mem_t interface_list;

static void
dump_class_node(struct RBNode *tree)
{
	printf("class : %s\n", to_class(tree)->class_name);
	if(tree->lchild != NULL)
		dump_class_node(tree->lchild);
	if(tree->rchild != NULL)
		dump_class_node(tree->rchild);
}

void
dump_class_tree()
{
	if(rbGetRoot(class_tree) != NULL)
		dump_class_node(rbGetRoot(class_tree));
}

//TODO:interface
static void
swapClass(struct RBNode *src, struct RBNode *dst)
{
	const char *tmp_name = to_class(src)->class_name;
	struct ZObjClass *tmp_parent = to_class(src)->parent;
	void (*tmp_cons)(struct ZObjInstance *, void *) = to_class(src)->constructor;
	void (*tmp_des)(struct ZObjInstance *) = to_class(src)->destructor;
	mem_t tmp_body = to_class(src)->class_body;
	
	to_class(src)->class_name = to_class(dst)->class_name;
	to_class(src)->parent = to_class(dst)->parent;
	to_class(src)->constructor = to_class(dst)->constructor;
	to_class(src)->destructor = to_class(dst)->destructor;
	to_class(src)->class_body = to_class(dst)->class_body;
	
	to_class(dst)->class_name = tmp_name;
	to_class(dst)->parent = tmp_parent;
	to_class(dst)->constructor = tmp_cons;
	to_class(dst)->destructor = tmp_des;
	to_class(dst)->class_body = tmp_body;
}

static void *
getClass(struct RBNode *p)
{
	return to_class(p);
}

static int
cmpClass(void *n1, void *n2)
{
	return strcmp(((struct ZObjClass *)n1)->class_name, ((struct ZObjClass *)n2)->class_name);
}

static struct RBNode *
makeClass(void *class_info)
{
	struct ZObjClass *class = malloc(sizeof(struct ZObjClass));
	assert(class != NULL);
	*class = *(struct ZObjClass *)class_info;
	return &class->rb_node;
}

static void
freeClass(struct RBNode *node)
{
	free(to_class(node));
}

void
zObjInit()
{
	class_tree = makeRBTree(getClass, cmpClass, makeClass, swapClass, freeClass);
}

static struct ZObjClass *
find_class(const char *name)
{
	struct ZObjClass tmp_class = { name };
	struct RBNode *node = rbSearch(class_tree, &tmp_class);
	if(node == NULL)
		return NULL;
	return to_class(node);
}

int	
zRegistClass(const char *class_name, const char *parent_name, void (*cons)(struct ZObjInstance *, void *),
	void (*des)(struct ZObjInstance *), void *class_body, unsigned int size_of_class)
{
	mem_t class = makeMem(size_of_class);
	struct ZObjClass *parent = NULL;
	if(find_class(class_name) != NULL) {
		printf("CRITICAL: redefined class %s.\n", class_name);
		return -1;
	}
	if(parent_name != NULL) {
		parent = find_class(parent_name);
		if(parent == NULL)
			printf("CRITICAL: parent not found.\n");
	}
	if(class_body != NULL)
		memCpy(&class, class_body, size_of_class);
	struct ZObjClass  reg_class = { class_name, parent, cons, des, class };
	rbInsert(&class_tree, &reg_class);
	return 0;
}

struct ZObjInstance *
zNewInstance(const char *class_name, void *data)
{
	struct ZObjClass *dst_class = find_class(class_name);
	if(opt_unlikely(dst_class == NULL)) {
		printf("CRITICAL: class %s not found.\n", class_name);
		return NULL;
	}
	mem_t ins = makeMem(sizeof(struct ZObjInstance));
	GET_TYPE_MEM(&ins, struct ZObjInstance, 0)->class = dst_class;
	dst_class->constructor(getMemPtr(&ins, 0, 0), data);
	if(dst_class->parent != NULL) {
		if(getMemIndex(GET_TYPE_MEM(&ins, struct ZObjInstance, 0)->parent) == 0) {
			GET_TYPE_MEM(&ins, struct ZObjInstance, 0)->parent = makeMem(sizeof(struct ZObjInstance));
			ADD_ITEM(&GET_TYPE_MEM(&ins, struct ZObjInstance, 0)->parent, struct ZObjInstance, 
				*zNewInstance(dst_class->parent->class_name, NULL));
		}
	}
	return getMemPtr(&ins, 0, 0);
}

void *
zGetClass(struct ZObjInstance *ins, const char *class_name)
{
	struct ZObjClass *class = ins->class;
	if(class_name != NULL) {
		while(strcmp(class->class_name, class_name) != 0) {
			class = class->parent;
			if(class == NULL) {
				printf("CRITICAL: class %s not found.\n", class_name);
				return NULL;
			}
		}
	}
	return getMemPtr(&class->class_body, 0, 0);
}

void *
zGetInstance(struct ZObjInstance *ins, const char *class_name)
{
	if(class_name != NULL) {
		while(strcmp(ins->class->class_name, class_name) != 0) {
			if(ins->class->parent == NULL) {
				printf("CRITICAL: class %s not found.\n", class_name);
				return NULL;
			}
			ins = getMemPtr(&ins->parent, 0, 0);
		}
	}
	return getMemPtr(&ins->instance_body, 0, 0);
}