/*
 * Copyright © 2018 Valve Corporation
 * Copyright © 2018 Google
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
 */

#include "aco_ir.h"

/*
 * Insert p_linear_start instructions right before RA to correctly allocate
 * temporaries for reductions that have to disrespect EXEC by executing in
 * WWM.
 */

namespace aco {

void setup_reduce_temp(Program* program)
{
   unsigned nesting_depth = 0;
   unsigned last_top_level_block_idx = 0;

   unsigned maxSize = 0;

   for (std::unique_ptr<Block>& block : program->blocks) {
      for (aco_ptr<Instruction>& instr : block->instructions) {
         if (instr->format != Format::PSEUDO_REDUCTION)
            continue;

         maxSize = MAX2(maxSize, instr->getOperand(0).size());
      }
   }

   if (maxSize == 0)
      return;

   assert(maxSize == 1 || maxSize == 2);
   Temp reduceTmp{program->allocateId(), maxSize == 2 ? v2_linear : v1_linear};
   bool inserted = false;

   for (std::unique_ptr<Block>& block : program->blocks) {
      if (block->loop_nest_depth == 0 && block->linear_predecessors.size() == 2)
         nesting_depth--;
      if (block->loop_nest_depth == 0 && nesting_depth == 0) {
         last_top_level_block_idx = block->index;
      }

      std::vector<aco_ptr<Instruction>>::iterator it;
      for (it = block->instructions.begin(); it != block->instructions.end(); ++it) {
         if ((*it)->format != Format::PSEUDO_REDUCTION)
            continue;

         if (!inserted) {
            aco_ptr<Instruction> create{create_instruction<Instruction>(aco_opcode::p_start_linear_vgpr, Format::PSEUDO, 0, 1)};
            create->getDefinition(0) = Definition(reduceTmp);
            /* find the right place to insert this definition */
            if (last_top_level_block_idx == block->index) {
               /* insert right before the current instruction */
               it = block->instructions.insert(it, std::move(create));
               it++;
            } else {
               assert(last_top_level_block_idx < block->index);
               /* insert before the branch at last top level block */
               std::vector<aco_ptr<Instruction>>& instructions = program->blocks[last_top_level_block_idx]->instructions;
               instructions.insert(std::next(instructions.begin(), instructions.size() - 1), std::move(create));
            }
         }

         Temp val = reduceTmp;
         if (val.size() != (*it)->getOperand(0).size()) {
            val = Temp{program->allocateId(), linearClass((*it)->getOperand(0).regClass())};
            aco_ptr<Instruction> split{create_instruction<Instruction>(aco_opcode::p_split_vector, Format::PSEUDO, 1, 2)};
            split->getOperand(0) = Operand(reduceTmp);
            split->getDefinition(0) = Definition(val);
            it = block->instructions.insert(it, std::move(split));
            it++;
         }

         (*it)->getOperand(1) = Operand(val);
      }

      if (block->loop_nest_depth == 0 && block->linear_successors.size() == 2)
         nesting_depth++;
   }
}

};
