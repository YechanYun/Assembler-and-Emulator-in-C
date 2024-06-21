#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>

#include "ast.h"
#include "ast_to_ir.h"
#include "eval.h"
#include "ir.h"
#include "utils_ir.h"

extern int registers[NUM_REGISTERS];

static void Statements_to_IR(IRProgram *program, Statements *statements, State *state, int *line, int count_update);


static void AssignmentStmt_to_IR(IRProgram *program, AssignmentStmt *assign_stmt, State *state, int *line, int count_update)
{
    uint8_t expr_reg = evalExpression(program, assign_stmt->expression, state, line, count_update);
    uint8_t reg = (strcmp(assign_stmt->name, "ret") == 0)
                ? X0 : getRegisterState(create_name(assign_stmt->name), state);
    IRInstruction *instr = create_ir_instruction(IR_MOV, reg, expr_reg, NOT_USED, NOT_USED, line);
    instr->dest->type = REG;
    instr->src1->type = REG;
    insertInstruction(program, instr, count_update);
    updateState(state, reg, registers[expr_reg]);
    // Support only "=" now
}

static void FlowStmt_to_IR(IRProgram *program, FlowStmt *flow_stmt, State *state, int *line, int count_update)
{
    if (strcmp(flow_stmt->name, "return") == 0) { // Return statement
        if (flow_stmt->expression != NULL) {
            AssignmentStmt *assign = create_assignment_stmt("ret", flow_stmt->expression);
            AssignmentStmt_to_IR(program, assign, state, line, count_update);
        }
        // Pop return address in register X30 and jump to it
        pop_from_stack(program, state, RP, line);
        IRInstruction *branchToFunctionCall = create_ir_instruction(IR_BR, RP, NOT_USED, NOT_USED, NOT_USED, line);
        branchToFunctionCall->dest->type = REG;
        insertInstruction(program, branchToFunctionCall, count_update);
    } else { // Break / Continue statement
        // IRInstruction *breakInstruction = create_ir_instruction(IR_B, LOOP_COND, NOT_USED, NOT_USED, NOT_USED, line);
        // insertInstruction(program, breakInstruction);
    }
}


static void ForStmt_to_IR(IRProgram *program, ForStmt *for_stmt, State *state, int *line, int count_update)
{
    Arguments *args = for_stmt->range->function_call->args;
    assert(args != NULL); // low_bound
    assert(args->next != NULL); // upp_bound
    int low_bound = evalExpression(program, args->arg, state, line, count_update);
    int upp_bound = evalExpression(program, args->next->arg, state, line, count_update);
    // function_call is of form:    range(low_bound, upp_bound)

    // Assignment
    AssignmentStmt *assign = create_assignment_stmt(for_stmt->var, args->arg);
    AssignmentStmt_to_IR(program, assign, state, line, count_update);

    // Condition check
    int line_to_return = *line;
    uint8_t reg = getRegisterState(create_name(assign->name), state);
    IRInstruction *condition = create_ir_instruction(IR_CMP, reg, upp_bound, NOT_USED, NOT_USED, line);
    condition->dest->type = REG;
    condition->src1->type = IMM;
    insertInstruction(program, condition, count_update);
    IRInstruction *condition2 = create_ir_instruction(IR_BCOND, B_GE, NOT_USED, NOT_USED, NOT_USED, line);
    condition2->dest->type = LABEL;
    insertInstruction(program, condition2, count_update);

    // Convert block statements to IR
    Statements_to_IR(program, for_stmt->block, state, line, count_update + upp_bound - low_bound);

    // Update counter
    int r = getRegisterState(create_name(assign->name), state);
    IRInstruction *update_counter = create_ir_instruction(IR_ADD, r, r, 1, NOT_USED, line);
    update_counter->dest->type = REG;
    update_counter->src1->type = REG;
    update_counter->src2->type = IMM;
    insertInstruction(program, update_counter, count_update);

    // Unconditional branch to condition check
    IRInstruction *branch_uncond = create_ir_instruction(IR_B, line_to_return, NOT_USED, NOT_USED, NOT_USED, line);
    branch_uncond->dest->type = LABEL;
    insertInstruction(program, branch_uncond, count_update);

    // Update branch condition location
    condition2->src1->reg = *line;
    condition2->src1->type = LABEL;
}

static void WhileStmt_to_IR(IRProgram *program, WhileStmt *while_stmt, State *state, int *line, int count_update)
{
    assert(while_stmt->condition->tag == EXPR_BINARY_OP);

    // Condition check
    int line_to_return = *line;
    BinaryOp *binary_op = while_stmt->condition->binary_op;
    BranchConditional op = getComparison(binary_op);
    uint8_t left_op = evalExpression(program, binary_op->left, state, line, count_update);
    uint8_t right_op = evalExpression(program, binary_op->right, state, line, count_update);
    IRInstruction *condition = create_ir_instruction(IR_CMP, left_op, right_op, NOT_USED, NOT_USED, line);
    condition->dest->type = REG;
    condition->src1->type = REG;
    insertInstruction(program, condition, count_update);
    IRInstruction *condition2 = create_ir_instruction(IR_BCOND, op, NOT_USED, NOT_USED, NOT_USED, line);
    condition2->dest->type = LABEL;
    insertInstruction(program, condition2, count_update);

    // Convert block statements to IR
    Statements_to_IR(program, while_stmt->block, state, line, count_update);

    // Unconditional branch to condition check
    IRInstruction *branch_uncond = create_ir_instruction(IR_B, line_to_return, NOT_USED, NOT_USED, NOT_USED, line);
    branch_uncond->dest->type = LABEL;
    insertInstruction(program, branch_uncond, count_update);

    // Update branch condition location
    condition2->src1->reg = *line;
    condition2->src1->type = LABEL;
}

static void IfStmt_to_IR(IRProgram *program, IfStmt *if_stmt, State *state, int *line, int count_update)
{
    // Condition check
    BinaryOp *binary_op = if_stmt->condition->binary_op;
    BranchConditional op = getNegatedComparison(binary_op);
    int left_op = evalExpression(program, binary_op->left, state, line, count_update);
    int right_op = evalExpression(program, binary_op->right, state, line, count_update);
    IRInstruction *condition = create_ir_instruction(IR_CMP, left_op, right_op, NOT_USED, NOT_USED, line);
    condition->dest->type = REG;
    condition->src1->type = REG;
    insertInstruction(program, condition, count_update);
    IRInstruction *condition2 = create_ir_instruction(IR_BCOND, op, NOT_USED, NOT_USED, NOT_USED, line);
    condition2->dest->type = LABEL;
    insertInstruction(program, condition2, count_update);

    // Then block
    Statements_to_IR(program, if_stmt->then_block, state, line, count_update);

    // Unconditional branch past the else block
    IRInstruction *branch_uncond = create_ir_instruction(IR_B, NOT_USED, NOT_USED, NOT_USED, NOT_USED, line);
    insertInstruction(program, branch_uncond, count_update);

    // Update branch condition location for the else block
    condition2->src1->reg = *line;

    // Else block
    Statements_to_IR(program, if_stmt->else_block, state, line, count_update);

    // Update the unconditional branch to skip the else block
    branch_uncond->dest->reg = *line;
    branch_uncond->dest->type = LABEL;
}

static void FunctionDef_to_IR(IRProgram *program, FunctionDef *function_def, State *state, int *line, int count_update)
{
    // Create instructions for function parameters
    Parameters *param = function_def->parameters;
    int param_count = 1;
    while (param != NULL) {
        addNameState(state, param_count++, param->parameter->name);
        param = param->next;
    }

    // Convert function body statements to IR
    Statements_to_IR(program, function_def->body, state, line, count_update);

    // Returning is handle by the return statement
}

static void Statements_to_IR(IRProgram *program, Statements *statements, State *state, int *line, int count_update)
{
    while(statements != NULL) {
        switch(statements->statement->tag) {
            case ASSIGNMENT_STMT: {
                AssignmentStmt *assignment_stmt = statements->statement->assignment_stmt;
                AssignmentStmt_to_IR(program, assignment_stmt, state, line, count_update);
                break;
            }
            case FLOW_STMT: {
                FlowStmt *flow_stmt = statements->statement->flow_stmt;
                FlowStmt_to_IR(program, flow_stmt, state, line, count_update);
                break;
            }
            case FOR_STMT: {
                ForStmt *for_stmt = statements->statement->for_stmt;
                ForStmt_to_IR(program, for_stmt, state, line, count_update);
                break;
            }
            case WHILE_STMT: {
                WhileStmt *while_stmt = statements->statement->while_stmt;
                WhileStmt_to_IR(program, while_stmt, state, line, count_update);
                break;
            }
            case IF_STMT: {
                IfStmt *if_stmt = statements->statement->if_stmt;
                IfStmt_to_IR(program, if_stmt, state, line, count_update);
                break;
            }
            case FUNCTION_DEF: {
                FunctionDef *function_definition = statements->statement->function_def;
                State *callee_state = create_state();
                FunctionDef_to_IR(program, function_definition, callee_state, line, count_update);
                break;
            }
        }
        statements = statements->next;
    }
}


IRProgram *AST_to_IR(Program *prog)
{
    // Create an IR program
    IRProgram *program = create_ir_program();
    State *state = create_state();

    for(int i = 0; i < NUM_REGISTERS; i++) {
        registers[i] = NOT_USED;
    }

    // Set up SP
    int line = 0;
    IRInstruction *set_sp = create_ir_instruction(IR_MOVZ, SP, state->stack_size, NOT_USED, NOT_USED, &line);
    set_sp->dest->type = REG;
    set_sp->src1->type = IMM;
    insertInstruction(program, set_sp, 1);

    Statements_to_IR(program, prog->statements, state, &line, 1);

    return program;
}
