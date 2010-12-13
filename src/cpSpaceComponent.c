/* Copyright (c) 2007 Scott Lembcke
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
 
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "chipmunk_private.h"

#pragma mark Sleeping Functions

// Chipmunk uses a data structure called a disjoint set forest.
// I was hoping it was possible to splice circularly linked lists
// in constant time, but it turns out it's not. I found this neat data structure instead.

static inline cpBody *
componentNodeRoot(cpBody *body)
{
	cpBody *parent = body->node.parent;
	
	if(parent){
		// path compression, attaches this node directly to the root
		return (body->node.parent = componentNodeRoot(parent));
	} else {
		return body;
	}
}

static inline void
componentNodeMerge(cpBody *a_root, cpBody *b_root)
{
	if(a_root->node.rank < b_root->node.rank){
		a_root->node.parent = b_root;
	} else if(a_root->node.rank > b_root->node.rank){
		b_root->node.parent = a_root;
	} else if(a_root != b_root){
		b_root->node.parent = a_root;
		a_root->node.rank++;
	}
}

void
cpSpaceActivateBody(cpSpace *space, cpBody *body)
{
	if(space->locked){
		// cpSpaceActivateBody() is called again once the space is unlocked
		cpArrayPush(space->rousedBodies, body);
	} else {
		cpArrayPush(space->bodies, body);
		for(cpShape *shape=body->shapeList; shape; shape=shape->next){
			cpSpatialIndexRemove(space->staticShapes, shape, shape->hashid);
			cpSpatialIndexInsert(space->activeShapes, shape, shape->hashid);
		}
		
		for(cpArbiter *arb = body->arbiterList; arb; arb = (arb->a->body == body ? arb->nextA : arb->nextB)){
			if(body == arb->a->body || cpBodyIsStatic(arb->a->body)){
				int numContacts = arb->numContacts;
				cpContact *contacts = arb->contacts;
				
				// Restore contact values back to the space's contact buffer memory
				arb->contacts = cpContactBufferGetArray(space);
				memcpy(arb->contacts, contacts, numContacts*sizeof(cpContact));
				cpSpacePushContacts(space, numContacts);
				
				cpShape *a = arb->a, *b = arb->b;
				cpShape *shape_pair[] = {a, b};
				cpHashValue arbHashID = CP_HASH_PAIR((size_t)a, (size_t)b);
				cpHashSetInsert(space->contactSet, arbHashID, shape_pair, arb, NULL);
				
				cpfree(contacts);
			}
		}
		
		for(cpConstraint *c = body->constraintList; c; c = (c->a == body ? c->nextA : c->nextB)){
			cpBody *other = c->a;
			if(other == body || cpBodyIsStatic(other)) cpArrayPush(space->constraints, c);
		}
	}
}

static void
cpSpaceDeactivateBody(cpSpace *space, cpBody *body)
{
	for(cpShape *shape = body->shapeList; shape; shape = shape->next){
		cpSpatialIndexRemove(space->activeShapes, shape, shape->hashid);
		cpSpatialIndexInsert(space->staticShapes, shape, shape->hashid);
	}
	
	for(cpArbiter *arb = body->arbiterList; arb; arb = (arb->a->body == body ? arb->nextA : arb->nextB)){
		if(body == arb->a->body || cpBodyIsStatic(arb->a->body)){
			cpShape *a = arb->a, *b = arb->b;
			cpShape *shape_pair[] = {a, b};
			cpHashValue arbHashID = CP_HASH_PAIR((size_t)a, (size_t)b);
			cpHashSetRemove(space->contactSet, arbHashID, shape_pair);
			cpArrayDeleteObj(space->arbiters, arb);
			
			// Save contact values to a new block of memory so they won't time out
			size_t bytes = arb->numContacts*sizeof(cpContact);
			cpContact *contacts = (cpContact *)cpmalloc(bytes);
			memcpy(contacts, arb->contacts, bytes);
			arb->contacts = contacts;
		}
	}
		
	for(cpConstraint *c = body->constraintList; c; c = (c->a == body ? c->nextA : c->nextB)){
		cpBody *other = c->a;
		if(other == body || cpBodyIsStatic(other)) cpArrayDeleteObj(space->constraints, c);
	}
}

static inline void
componentActivate(cpBody *root)
{
	if(!cpBodyIsSleeping(root)) return;
	
	cpSpace *space = root->space;
	cpAssert(space, "Trying to activate a body that was never added to a space.");
	
	cpBody *body = root, *next;
	do {
		next = body->node.next;
		
		cpComponentNode node = {NULL, NULL, 0, 0.0f};
		body->node = node;
		
		cpSpaceActivateBody(space, body);
	} while((body = next) != root);
	
	cpArrayDeleteObj(space->sleepingComponents, root);
}

void
cpBodyActivate(cpBody *body)
{
	componentActivate(componentNodeRoot(body));
}

static inline void
mergeBodies(cpSpace *space, cpArray *components, cpArray *rogueBodies, cpBody *a, cpBody *b)
{
	// Ignore connections to static bodies
	if(cpBodyIsStatic(a) || cpBodyIsStatic(b)) return;
	
	cpBody *a_root = componentNodeRoot(a);
	cpBody *b_root = componentNodeRoot(b);
	
	cpBool a_sleep = cpBodyIsSleeping(a_root);
	cpBool b_sleep = cpBodyIsSleeping(b_root);
	
	if(a_sleep || b_sleep){
		componentActivate(a_root);
		componentActivate(b_root);
	} 
	
	// Add any rogue bodies found to the list and reset the idle time of anything they touch.
	if(cpBodyIsRogue(a)){ cpArrayPush(rogueBodies, a); b->node.idleTime = 0.0f; }
	if(cpBodyIsRogue(b)){ cpArrayPush(rogueBodies, b); a->node.idleTime = 0.0f; }
	
	componentNodeMerge(a_root, b_root);
}

static inline cpBool
componentActive(cpBody *root, cpFloat threshold)
{
	cpBody *body = root, *next;
	do {
		next = body->node.next;
		if(body->node.idleTime < threshold) return cpTrue;
	} while((body = next) != root);
	
	return cpFalse;
}

static inline void
addToComponent(cpBody *body, cpArray *components)
{
	// Check that the body is not already added to the component list
	if(body->node.next) return;
	cpBody *root = componentNodeRoot(body);
	
	cpBody *next = root->node.next;
	if(!next){
		// If the root isn't part of a list yet, then it hasn't been
		// added to the components list. Do that now.
		cpArrayPush(components, root);
		// Start the list
		body->node.next = root;
		root->node.next = body;
	} else if(root != body) {
		// Splice in body after the root.
		body->node.next = next;
		root->node.next = body;
	}
}

static inline void
cpBodyPushArbiter(cpBody *body, cpArbiter *arb)
{
	if(!cpBodyIsStatic(body) && !cpBodyIsRogue(body)){
		arb->nextA = body->arbiterList;
		body->arbiterList = arb;
	}
}

// TODO this function needs more commenting.
void
cpSpaceProcessComponents(cpSpace *space, cpFloat dt)
{
	cpArray *bodies = space->bodies;
	cpArray *newBodies = cpArrayNew(bodies->num);
	cpArray *rogueBodies = cpArrayNew(16);
	cpArray *arbiters = space->arbiters;
	cpArray *constraints = space->constraints;
	cpArray *components = cpArrayNew(space->sleepingComponents->num);
	
	cpFloat dv = space->idleSpeedThreshold;
	cpFloat dvsq = (dv ? dv*dv : cpvdot(space->gravity, space->gravity)*dt*dt);
	
	// update idling and reset arbiter lists
	for(int i=0; i<bodies->num; i++){
		cpBody *body = (cpBody*)bodies->arr[i];
		
		cpFloat thresh = (dvsq ? body->m*dvsq : 0.0f);
		body->node.idleTime = (cpBodyKineticEnergy(body) > thresh ? 0.0f : body->node.idleTime + dt);
		
		body->arbiterList = NULL;
	}
	
	// iterate graph edges and build forests
	for(int i=0, count=arbiters->num; i<count; i++){
		cpArbiter *arb = (cpArbiter*)arbiters->arr[i];
		mergeBodies(space, components, rogueBodies, arb->a->body, arb->b->body);
		
		// Push arbiter connectivity onto bodies
		cpBodyPushArbiter(arb->a->body, arb);
		cpBodyPushArbiter(arb->b->body, arb);
	}
	
	for(int j=0; j<constraints->num; j++){
		cpConstraint *constraint = (cpConstraint *)constraints->arr[j];
		mergeBodies(space, components, rogueBodies, constraint->a, constraint->b);
	}
	
	// iterate bodies and add them to their components
	for(int i=0; i<bodies->num; i++) addToComponent((cpBody*)bodies->arr[i], components);
	for(int i=0; i<rogueBodies->num; i++) addToComponent((cpBody*)rogueBodies->arr[i], components);
	
	// iterate components, copy or deactivate
	for(int i=0; i<components->num; i++){
		cpBody *root = (cpBody*)components->arr[i];
		cpBody *body = root, *next = NULL;
		if(componentActive(root, space->sleepTimeThreshold)){
			do {
				next = body->node.next;
				
				if(!cpBodyIsRogue(body)) cpArrayPush(newBodies, body);
				cpComponentNode node = {NULL, NULL, 0, body->node.idleTime};
				body->node = node;
			} while((body = next) != root);
		} else {
			do {
				next = body->node.next;
				cpSpaceDeactivateBody(space, body);
			} while((body = next) != root);
			
			cpArrayPush(space->sleepingComponents, root);
		}
	}
	
	space->bodies = newBodies;
	cpArrayFree(bodies);
	cpArrayFree(rogueBodies);
	cpArrayFree(components);
}

void
cpBodySleep(cpBody *body)
{
	cpBodySleepWithGroup(body, NULL);
}

void
cpBodySleepWithGroup(cpBody *body, cpBody *group){
	cpAssert(!cpBodyIsStatic(body) && !cpBodyIsRogue(body), "Rogue and static bodies cannot be put to sleep.");
	
	cpSpace *space = body->space;
	cpAssert(space, "Cannot put a body to sleep that has not been added to a space.");
	cpAssert(!space->locked, "Bodies can not be put to sleep during a query or a call to cpSpaceSte(). Put these calls into a post-step callback.");
	cpAssert(!group || cpBodyIsSleeping(group), "Cannot use a non-sleeping body as a group identifier.");
	
	if(cpBodyIsSleeping(body)) return;
	
	for(cpShape *shape = body->shapeList; shape; shape = shape->next) cpShapeUpdate(shape, body->p, body->rot);
	cpSpaceDeactivateBody(space, body);
	
	if(group){
		cpBody *root = componentNodeRoot(group);
		
		cpComponentNode node = {root, root->node.next, 0, 0.0f};
		body->node = node;
		root->node.next = body;
	} else {
		cpComponentNode node = {NULL, body, 0, 0.0f};
		body->node = node;
		
		cpArrayPush(space->sleepingComponents, body);
	}
	
	cpArrayDeleteObj(space->bodies, body);
}

static void
activateTouchingHelper(cpShape *shape, cpContactPointSet *points, cpArray **bodies){
	cpBodyActivate(shape->body);
}

void
cpSpaceActivateShapesTouchingShape(cpSpace *space, cpShape *shape){
	cpArray *bodies = NULL;
	cpSpaceShapeQuery(space, shape, (cpSpaceShapeQueryFunc)activateTouchingHelper, &bodies);
}
