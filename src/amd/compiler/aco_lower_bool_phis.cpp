/*
 * Copyright © 2019 Valve Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Rhys Perry (pendingchaos02@gmail.com)
 *
 */

#include <map>
#include <utility>

#include "aco_ir.h"


namespace aco {

struct phi_use {
   Block *block;
   unsigned phi_def;

   bool operator<(const phi_use& other) const {
      return std::make_tuple(block, phi_def) <
             std::make_tuple(other.block, other.phi_def);
   }
};

struct ssa_state {
   std::map<Block *, unsigned> latest;
   std::map<unsigned, std::map<phi_use, uint64_t>> phis;
};

Operand get_ssa(Program *program, Block *block, ssa_state *state)
{
   Temp res;
   while (true) {
      auto pos = state->latest.find(block);
      if (pos != state->latest.end())
         return Operand({pos->second, s2});

      size_t pred = block->linear_predecessors.size();
      if (pred == 0) {
         return Operand();
      } else if (pred == 1) {
         block = block->linear_predecessors[0];
         continue;
      } else {
         unsigned res = program->allocateId();
         state->latest[block] = res;

         aco_ptr<Instruction> phi{create_instruction<Instruction>(aco_opcode::p_linear_phi, Format::PSEUDO, pred, 1)};
         for (unsigned i = 0; i < pred; i++) {
            phi->getOperand(i) = get_ssa(program, block->linear_predecessors[i], state);
            if (phi->getOperand(i).isTemp()) {
               assert(i < 64);
               state->phis[phi->getOperand(i).tempId()][(phi_use){block, res}] |= (uint64_t)1 << i;
            }
         }
         phi->getDefinition(0) = Definition(Temp{res, s2});
         block->instructions.emplace(block->instructions.begin(), std::move(phi));

         return Operand({res, s2});
      }
   }
}

void update_phi(Program *program, ssa_state *state, Block *block, unsigned phi_def, uint64_t operand_mask) {
   for (auto& phi : block->instructions) {
      if (phi->opcode != aco_opcode::p_phi && phi->opcode != aco_opcode::p_linear_phi)
         break;
      if (phi->opcode != aco_opcode::p_linear_phi)
         continue;
      if (phi->getDefinition(0).tempId() != phi_def)
         continue;
      assert(ffsll(operand_mask) <= phi->operandCount());

      uint64_t operands = operand_mask;
      while (operands) {
         unsigned operand = u_bit_scan64(&operands);
         Operand new_operand = get_ssa(program, block->linear_predecessors[operand], state);
         phi->getOperand(operand) = new_operand;
         if (!new_operand.isUndefined())
            state->phis[new_operand.tempId()][(phi_use){block, phi_def}] |= (uint64_t)1 << operand;
      }
      return;
   }
   assert(false);
}

Temp write_ssa(Program *program, Block *block, ssa_state *state, unsigned previous) {
   unsigned id = program->allocateId();
   state->latest[block] = id;

   /* update phis */
   if (previous) {
      std::map<phi_use, uint64_t> phis;
      phis.swap(state->phis[previous]);
      for (auto& phi : phis)
         update_phi(program, state, phi.first.block, phi.first.phi_def, phi.second);
   }

   return {id, s2};
}

void insert_before_branch(Block *block, aco_ptr<Instruction> instr)
{
   int end = block->instructions.size() - 1;
   if (block->instructions[end]->format == Format::PSEUDO_BRANCH)
      block->instructions.emplace(std::prev(block->instructions.end()), std::move(instr));
   else
      block->instructions.emplace_back(std::move(instr));
}

void insert_before_logical_end(Block *block, aco_ptr<Instruction> instr)
{
   for (int i = block->instructions.size() - 1; i >= 0; --i) {
      if (block->instructions[i]->opcode == aco_opcode::p_logical_end) {
         block->instructions.emplace(std::next(block->instructions.begin(), i), std::move(instr));
         return;
      }
   }
   insert_before_branch(block, std::move(instr));
}

aco_ptr<Instruction> lower_divergent_bool_phi(Program *program, Block *block, aco_ptr<Instruction>& phi)
{
   ssa_state state;
   for (unsigned i = 0; i < phi->operandCount(); i++) {
      Block *pred = block->logical_predecessors[i];

      assert(phi->getOperand(i).isTemp());
      Temp phi_src = phi->getOperand(i).getTemp();
      if (phi_src.regClass() == s1) {
         aco_ptr<Instruction> cselect{create_instruction<SOP2_instruction>(aco_opcode::s_cselect_b64, Format::SOP2, 3, 1)};
         cselect->getOperand(0) = Operand((uint32_t) -1);
         cselect->getOperand(1) = Operand((uint32_t) 0);
         cselect->getOperand(2) = Operand(phi_src);
         cselect->getOperand(2).setFixed(scc);
         phi_src = {program->allocateId(), s2};
         cselect->getDefinition(0) = Definition(phi_src);
         insert_before_logical_end(pred, std::move(cselect));
      }
      assert(phi_src.regClass() == s2);

      Operand cur = get_ssa(program, pred, &state);
      Temp new_cur = write_ssa(program, pred, &state, cur.tempId());

      if (cur.isUndefined()) {
         aco_ptr<Instruction> merge{create_instruction<SOP1_instruction>(aco_opcode::s_mov_b64, Format::SOP1, 1, 1)};
         merge->getOperand(0) = Operand(phi_src);
         merge->getDefinition(0) = Definition(new_cur);
         insert_before_logical_end(pred, std::move(merge));
      } else {
         aco_ptr<Instruction> merge{create_instruction<SOP2_instruction>(aco_opcode::s_andn2_b64, Format::SOP2, 2, 2)};
         merge->getOperand(0) = cur;
         merge->getOperand(1) = Operand(exec, s2);
         Temp tmp1{program->allocateId(), s2};
         merge->getDefinition(0) = Definition(tmp1);
         merge->getDefinition(1) = Definition(program->allocateId(), scc, b);
         insert_before_logical_end(pred, std::move(merge));

         merge.reset(create_instruction<SOP2_instruction>(aco_opcode::s_and_b64, Format::SOP2, 2, 2));
         merge->getOperand(0) = Operand(phi_src);
         merge->getOperand(1) = Operand(exec, s2);
         Temp tmp2{program->allocateId(), s2};
         merge->getDefinition(0) = Definition(tmp2);
         merge->getDefinition(1) = Definition(program->allocateId(), scc, b);
         insert_before_logical_end(pred, std::move(merge));

         merge.reset(create_instruction<SOP2_instruction>(aco_opcode::s_or_b64, Format::SOP2, 2, 2));
         merge->getOperand(0) = Operand(tmp1);
         merge->getOperand(1) = Operand(tmp2);
         merge->getDefinition(0) = Definition(new_cur);
         merge->getDefinition(1) = Definition(program->allocateId(), scc, b);
         insert_before_logical_end(pred, std::move(merge));
      }
   }

   aco_ptr<Instruction> copy{create_instruction<SOP1_instruction>(aco_opcode::s_mov_b64, Format::SOP1, 1, 1)};
   copy->getOperand(0) = Operand(get_ssa(program, block, &state));
   copy->getDefinition(0) = phi->getDefinition(0);
   return copy;
}

void lower_bool_phis(Program* program)
{
   for (std::vector<std::unique_ptr<Block>>::iterator it = program->blocks.begin(); it != program->blocks.end(); ++it)
   {
      Block* block = it->get();
      std::vector<aco_ptr<Instruction>> instructions;
      std::vector<aco_ptr<Instruction>> non_phi;
      instructions.swap(block->instructions);
      block->instructions.reserve(instructions.size());
      unsigned i = 0;
      for (; i < instructions.size(); i++)
      {
         aco_ptr<Instruction>& phi = instructions[i];
         if (phi->opcode != aco_opcode::p_phi && phi->opcode != aco_opcode::p_linear_phi)
            break;
         if (phi->opcode == aco_opcode::p_phi && phi->getDefinition(0).regClass() == s2) {
            non_phi.emplace_back(std::move(lower_divergent_bool_phi(program, block, phi)));
         } else {
            block->instructions.emplace_back(std::move(phi));
         }
      }
      for (auto&& instr : non_phi) {
         assert(instr->opcode != aco_opcode::p_phi && instr->opcode != aco_opcode::p_linear_phi);
         block->instructions.emplace_back(std::move(instr));
      }
      for (; i < instructions.size(); i++) {
         aco_ptr<Instruction> instr = std::move(instructions[i]);
         assert(instr->opcode != aco_opcode::p_phi && instr->opcode != aco_opcode::p_linear_phi);
         block->instructions.emplace_back(std::move(instr));
      }
   }
}

}