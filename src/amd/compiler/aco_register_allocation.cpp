/*
 * Copyright © 2018 Valve Corporation
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
 *    Daniel Schürmann (daniel.schuermann@campus.tu-berlin.de)
 *    Bas Nieuwenhuizen (bas@basnieuwenhuizen.nl)
 *
 */

#include <algorithm>
#include <map>
#include <unordered_map>
#include <set>
#include <functional>
#include <bitset>

#include "aco_ir.h"
#include "sid.h"


namespace aco {


void register_allocation(Program *program)
{

   /* calculate max register bounds */
   unsigned max_sgpr = 0;
   unsigned max_vgpr = 0;
   std::vector<std::set<Temp>> live_out_per_block = live_temps_at_end_of_block(program);

   assert(program->vgpr_demand <= 256 && program->sgpr_demand <= 100);
   if (program->vgpr_demand <= 24 && program->sgpr_demand <= 46) {
      max_sgpr = 46;
      max_vgpr = 24;
   } else if (program->vgpr_demand <= 28 && program->sgpr_demand <= 54) {
      max_sgpr = 54;
      max_vgpr = 28;
   } else if (program->vgpr_demand <= 32 && program->sgpr_demand <= 62) {
      max_sgpr = 62;
      max_vgpr = 32;
   } else if (program->vgpr_demand <= 36 && program->sgpr_demand <= 70) {
      max_sgpr = 70;
      max_vgpr = 36;
   } else if (program->vgpr_demand <= 40 && program->sgpr_demand <= 78) {
      max_sgpr = 78;
      max_vgpr = 40;
   } else if (program->vgpr_demand <= 48 && program->sgpr_demand <= 94) {
      max_sgpr = 94;
      max_vgpr = 48;
   } else {
      max_sgpr = 100;
      if (program->vgpr_demand <= 64)
         max_vgpr = 64;
      else if (program->vgpr_demand <= 84)
         max_vgpr = 84;
      else if (program->vgpr_demand <= 128)
         max_vgpr = 128;
      else
         max_vgpr = 256;
   }
   program->config->num_vgprs = max_vgpr;
   program->config->num_sgprs = max_sgpr + 2;

   std::unordered_map<unsigned, std::pair<PhysReg, RegClass>> assignments;
   std::vector<std::unordered_map<unsigned, Temp>> renames(program->blocks.size());
   std::map<unsigned, Temp> orig_names;

   std::function<std::pair<PhysReg, bool>(std::array<uint32_t, 512>&, std::vector<std::pair<Operand, Definition>>&,
                                          uint32_t, uint32_t, uint32_t, uint32_t, uint32_t)> _get_reg =
                                      [&](std::array<uint32_t, 512>& reg_file, std::vector<std::pair<Operand, Definition>>& pc,
                                          uint32_t lb, uint32_t ub, uint32_t size, uint32_t stride, uint32_t num_moves) {

      assert(num_moves <= size); // FIXME: extend this algorithm
      unsigned reg_lo = lb;
      unsigned reg_hi = lb + size - 1;
      /* trivial case: without moves */
      if (num_moves == 0) {
         bool found = false;
         while (!found && reg_lo + size <= ub) {
            if (reg_file[reg_lo] != 0) {
               reg_lo += stride;
               continue;
            }
            found = true;
            for (unsigned i = 1; i < size; i++) {
               reg_hi = reg_lo + i;
               if (reg_file[reg_hi] != 0) {
                  found = false;
                  break;
               }
            }
            if (found) {

               return std::make_pair(PhysReg{reg_lo}, true);
            }
            while (reg_lo <= reg_hi)
               reg_lo += stride;
         }
         return std::make_pair(PhysReg{0}, false);
      }

      /* we use a sliding window to find potential positions */
      for (reg_lo = lb, reg_hi = lb + size - 1; reg_hi < ub; reg_lo += stride, reg_hi += stride) {
         /* first check the edges: this is what we have to fix to allow for num_moves > size */
         if (reg_lo > lb + 1 && reg_file[reg_lo] == reg_file[reg_lo - 1])
            continue;
         if (reg_hi < ub - 1 && reg_file[reg_hi] == reg_file[reg_hi + 1])
            continue;

         /* second, check that we have at most k=num_moves elements in the window
          * and no element is larger than the currently processed one */
         unsigned k = 0;
         std::set<unsigned> vars;
         bool stop = false;
         for (unsigned j = reg_lo; j <= reg_hi; j++) {
            if (reg_file[j] == 0)
               continue;
            k++;
            /* 0xFFFF signals that this area is already blocked! */
            if (reg_file[j] == 0xFFFF || k > num_moves) {
               stop = true;
               break;
            }
            if (sizeOf(assignments[reg_file[j]].second) >= size) {
               stop = true;
               break;
            }
            vars.emplace(reg_file[j]);
         }
         if (stop)
            continue;

         /* now, we have a list of vars, we want to move away from the current slot */
         /* copy the current register file */
         std::array<uint32_t, 512> register_file = reg_file;
         /* mark the area as blocked: [reg_lo, reg_hi] */
         for (unsigned j = reg_lo; j <= reg_hi; j++)
            register_file[j] = 0xFFFF;

         std::vector<std::pair<Operand, Definition>> parallelcopy;
         bool success = true;
         unsigned remaining_moves = num_moves - k;
         for (unsigned id : vars) {
            std::pair<PhysReg, RegClass> var = assignments[id];
            uint32_t stride = 1;
            if (typeOf(var.second) == sgpr) {
               if (sizeOf(var.second) == 2)
                  stride = 2;
               if (sizeOf(var.second) > 3)
                  stride = 4;
            }
            unsigned num_moves = 0;
            std::pair<PhysReg, bool> res = _get_reg(register_file, parallelcopy, lb, ub, sizeOf(var.second), stride, num_moves);
            success &= res.second;
            while (!success && remaining_moves > 0) {
               remaining_moves--;
               num_moves++;
               res = _get_reg(register_file, parallelcopy, lb, ub, sizeOf(var.second), stride, num_moves);
               success &= res.second;
            }
            if (!success)
               break;
            /* mark the area as blocked */
            for (unsigned i = res.first.reg; i < res.first.reg + sizeOf(var.second); i++)
               register_file[i] = 0xFFFF;

            /* create parallelcopy pair (without definition id) */
            Temp tmp = Temp(id, var.second);
            Operand pc_op = Operand(tmp);
            pc_op.setFixed(var.first);
            Definition pc_def = Definition(res.first, pc_op.regClass());
            parallelcopy.emplace_back(pc_op, pc_def);
         }
         if (success) {
            /* if everything worked out: insert parallelcopies, release [reg_lo,reg_hi], copy back reg_file */
            pc.insert(pc.end(), parallelcopy.begin(), parallelcopy.end());
            reg_file = register_file;
            for (unsigned i = reg_lo; i < reg_lo + size; i++)
               reg_file[i] = 0;
            return std::make_pair(PhysReg{reg_lo}, true);
         }
      }

      return std::make_pair(PhysReg{0}, false);
   };

   std::function<PhysReg(std::array<uint32_t, 512>&, RegClass, std::vector<std::pair<Operand, Definition>>&,
                         std::unique_ptr<Instruction>&)> get_reg =
                     [&](std::array<uint32_t, 512>& reg_file, RegClass rc, std::vector<std::pair<Operand, Definition>>& pc,
                         std::unique_ptr<Instruction>& instr) {
      unsigned size = sizeOf(rc);
      uint32_t stride = 1;
      uint32_t lb, ub;
      if (typeOf(rc) == vgpr) {
         lb = 256;
         ub = 256 + max_vgpr;
      } else {
         lb = 0;
         ub = max_sgpr;
         if (sizeOf(rc) == 2)
            stride = 2;
         else if (sizeOf(rc) >= 4)
            stride = 4;
      }
      /* try without moves */
      std::pair<PhysReg, bool> res = _get_reg(reg_file, pc, lb, ub, size, stride, 0);
      if (res.second)
         return res.first;

      /* didn't work out: try with 1 .. n moves */
      assert(size > 1);
      for (unsigned k = 1; k <= size; k++) {
         std::pair<PhysReg, bool> res = _get_reg(reg_file, pc, lb, ub, size, stride, k);
         if (res.second) {
            /* we set the definition regs == 0. the actual caller is responsible for correct setting */
            for (unsigned i = 0; i < size; i++)
               reg_file[res.first.reg + i] = 0;
            /* allocate id's and rename operands: this is done transparently here */
            for (std::pair<Operand, Definition>& copy : pc) {
               /* the definitions with id are not from this function and already handled */
               if (!copy.second.isTemp()) {
                  copy.second.setTemp(Temp(program->allocateId(), copy.second.regClass()));
                  assignments[copy.second.tempId()] = {copy.second.physReg(), copy.second.regClass()};
                  for (unsigned i = copy.second.physReg().reg; i < copy.second.physReg().reg + copy.second.size(); i++)
                     reg_file[i] = copy.second.tempId();
                  /* check if we moved an operand */
                  for (unsigned i = 0; i < instr->num_operands; i++) {
                     if (!instr->getOperand(i).isTemp())
                        continue;
                     if (instr->getOperand(i).tempId() == copy.first.tempId()) {
                        instr->getOperand(i).setTemp(copy.second.getTemp());
                        instr->getOperand(i).setFixed(copy.second.physReg());
                     }
                  }
               }
            }
            /* it might happen that something was moved to the position of an killed operand */
            for (unsigned i = 0; i < instr->num_operands; i++) {
               if (instr->getOperand(i).getTemp().type() == typeOf(rc) && instr->getOperand(i).isKill()) {
                  for (unsigned j = 0; j < instr->getOperand(i).size(); j++) {
                     /* if that is the case, we have to find another position for this operand */
                     if (reg_file[instr->getOperand(i).physReg().reg + j] != 0) {
                        Operand op = instr->getOperand(i);
                        Definition def = Definition(program->allocateId(), op.regClass());
                        PhysReg reg = get_reg(reg_file, op.regClass(), pc, instr);
                        def.setFixed(reg);
                        pc.emplace_back(op, def);
                        instr->getOperand(i).setTemp(def.getTemp());
                        instr->getOperand(i).setFixed(reg);
                        break;
                     }
                  }
               }
            }
            return res.first;
         }
      }
      assert(false);
   };

   struct phi_info {
      Instruction* phi;
      unsigned block_idx;
      std::set<Instruction*> uses;

   };
   bool filled[program->blocks.size()];
   bool sealed[program->blocks.size()];
   memset(filled, 0, sizeof filled);
   memset(sealed, 0, sizeof sealed);
   std::vector<std::vector<std::unique_ptr<Instruction>>> phis(program->blocks.size());
   std::vector<std::vector<std::unique_ptr<Instruction>>> incomplete_phis(program->blocks.size());
   std::map<unsigned, phi_info> phi_map;
   std::function<Temp(Temp,Block*)> read_variable;
   std::function<Temp(Temp,Block*)> read_variable_recursive;
   std::function<Temp(std::map<unsigned, phi_info>::iterator)> try_remove_trivial_phi;

   read_variable = [&](Temp val, Block* block) -> Temp {
      std::unordered_map<unsigned, Temp>::iterator it = renames[block->index].find(val.id());

      /* check if the variable got a name in the current block */
      if (it != renames[block->index].end()) {
         return it->second;
      /* if not, look up the predecessor blocks */
      } else {
         return read_variable_recursive(val, block);
      }
   };

   read_variable_recursive = [&](Temp val, Block* block) -> Temp {
      bool is_logical = val.type() == vgpr;
      std::vector<Block*>& preds = is_logical ? block->logical_predecessors : block->linear_predecessors;
      assert(preds.size() > 0);

      Temp new_val;
      if (!sealed[block->index]) {
         /* if the block is not sealed yet, we create an incomplete phi (which might later get removed again) */
         new_val = Temp{program->allocateId(), val.regClass()};
         aco_opcode opcode = is_logical ? aco_opcode::p_phi : aco_opcode::p_linear_phi;
         std::unique_ptr<Instruction> phi{create_instruction<Instruction>(opcode, Format::PSEUDO, preds.size(), 1)};
         phi->getDefinition(0) = Definition(new_val);
         phi->getDefinition(0).setFixed(assignments[val.id()].first);
         assignments[new_val.id()] = {phi->getDefinition(0).physReg(), phi->getDefinition(0).regClass()};
         for (unsigned i = 0; i < preds.size(); i++)
            phi->getOperand(i) = Operand(val);

         phi_map.emplace(new_val.id(), phi_info{phi.get(), block->index});
         incomplete_phis[block->index].emplace_back(std::move(phi));

      } else if (preds.size() == 1) {
         /* if the block has only one predecessor, just look there for the name */
         new_val = read_variable(val, preds[0]);
      } else {
         /* if there are more predecessors, we create a phi just in case */
         new_val = Temp{program->allocateId(), val.regClass()};
         renames[block->index][val.id()] = new_val;
         aco_opcode opcode = is_logical ? aco_opcode::p_phi : aco_opcode::p_linear_phi;
         std::unique_ptr<Instruction> phi{create_instruction<Instruction>(opcode, Format::PSEUDO, preds.size(), 1)};

         phi->getDefinition(0) = Definition(new_val);
         phi->getDefinition(0).setFixed(assignments[val.id()].first);
         assignments[new_val.id()] = {phi->getDefinition(0).physReg(), phi->getDefinition(0).regClass()};
         phi_map.emplace(new_val.id(), phi_info{phi.get(), block->index});
         Instruction* instr = phi.get();

         /* we look up the name in all predecessors */
         for (unsigned i = 0; i < preds.size(); i++) {
            Temp op_temp = read_variable(val, preds[i]);
            instr->getOperand(i).setTemp(op_temp);
            assert(assignments.find(op_temp.id()) != assignments.end());
            instr->getOperand(i).setFixed(assignments[op_temp.id()].first);
            if (!(op_temp == new_val) && phi_map.find(op_temp.id()) != phi_map.end())
               phi_map[op_temp.id()].uses.emplace(instr);
         }
         /* we check if the phi is trivial (in which case we return the original value) */
         new_val = try_remove_trivial_phi(phi_map.find(new_val.id()));
         // TODO: that is quite inefficient, better keep temporaries because most phis are trivial
         // see the paper: we can mark visited blocks, and only emit a phi on second visit.
         // or better: we can try to detect cycles and only emit phi on loop headers
         phis[block->index].emplace_back(std::move(phi));
      }
      renames[block->index][val.id()] = new_val;
      orig_names[new_val.id()] = val;
      return new_val;
   };

   try_remove_trivial_phi = [&] (std::map<unsigned, phi_info>::iterator info) -> Temp {
      assert(info->second.block_idx != 0);
      Instruction* instr = info->second.phi;
      Temp same = Temp();
      Temp def = instr->getDefinition(0).getTemp();
      /* a phi node is trivial iff all operands are the same or the definition of the phi */
      for (unsigned i = 0; i < instr->num_operands; i++) {
         Temp op = instr->getOperand(i).getTemp();
         if (op == same || op == def)
            continue;
         if (!(same == Temp())) {
            /* phi is not trivial */
            return def;
         }
         same = op;
      }
      assert(!(same == Temp() || same == def));

      /* reroute all uses to same and remove phi */
      std::vector<std::map<unsigned, phi_info>::iterator> phi_users;
      for (Instruction* instr : info->second.uses) {
         for (unsigned i = 0; i < instr->num_operands; i++) {
            if (instr->getOperand(i).isTemp() && instr->getOperand(i).tempId() == def.id())
               instr->getOperand(i).setTemp(same);
         }
         /* recursively try to remove trivial phis */
         if (instr->opcode == aco_opcode::p_phi || instr->opcode == aco_opcode::p_linear_phi) {
            std::map<unsigned, phi_info>::iterator it = phi_map.find(instr->getDefinition(0).tempId());
            if (it != phi_map.end())
               phi_users.emplace_back(it);
         }
      }

      auto it = orig_names.find(same.id());
      unsigned orig_var = it != orig_names.end() ? it->second.id() : same.id();
      for (unsigned i = 0; i < program->blocks.size(); i++) {
         auto it = renames[i].find(orig_var);
         if (it != renames[i].end() && it->second == def)
            renames[i][orig_var] = same;
      }

      instr->num_definitions = 0; /* this indicates that the phi can be removed */
      phi_map.erase(info);
      for (auto it : phi_users)
         try_remove_trivial_phi(it);

      /* do to the removal of other phis, the name might have changed once again! */
      return renames[info->second.block_idx][orig_var];
   };

   std::map<unsigned, unsigned> affinities;
   std::vector<std::map<unsigned, Instruction*>> kills_per_block(program->blocks.size());
   for (std::vector<std::unique_ptr<Block>>::reverse_iterator it = program->blocks.rbegin(); it != program->blocks.rend(); it++) {
      std::unique_ptr<Block>& block = *it;

      /* first, compute the death points of all live vars within the block */
      std::set<Temp>& live = live_out_per_block[block->index];
      std::map<unsigned, Instruction*>& kills = kills_per_block[block->index];
      /* create dummy kill points for live outs */
      for (Temp t : live)
         kills.emplace(t.id(), nullptr);

      std::vector<std::unique_ptr<Instruction>>::reverse_iterator rit;
      for (rit = block->instructions.rbegin(); rit != block->instructions.rend(); ++rit) {
         std::unique_ptr<Instruction>& instr = *rit;
         if (instr->opcode != aco_opcode::p_linear_phi && instr->opcode != aco_opcode::p_phi) {
            for (unsigned i = 0; i < instr->num_operands; i++) {
               if (instr->getOperand(i).isTemp() && live.emplace(instr->getOperand(i).getTemp()).second)
                  kills.emplace(instr->getOperand(i).tempId(), instr.get());
            }
         } else {
            /* add affinities */
            unsigned preferred = instr->getDefinition(0).tempId();
            unsigned op_idx = instr->num_operands;
            std::vector<Block*>& preds = instr->opcode == aco_opcode::p_phi ? block->logical_predecessors : block->linear_predecessors;
            for (unsigned i = 0; i < instr->num_operands; i++) {
               if (preds[i]->index < block->index && instr->getOperand(i).isTemp() &&
                   instr->getOperand(i).tempId() < preferred &&
                   instr->getOperand(i).regClass() == instr->getDefinition(0).regClass()) {
                  assert(!instr->getOperand(i).isUndefined());
                  preferred = instr->getOperand(i).tempId();
                  op_idx = i;
               }
            }
            for (unsigned i = 0; i < instr->num_operands; i++) {
               if (instr->getOperand(i).isTemp() && i != op_idx)
                  affinities.emplace(instr->getOperand(i).tempId(), preferred);
            }
            if (op_idx < instr->num_operands)
               affinities.emplace(instr->getDefinition(0).tempId(), preferred);
         }
         for (unsigned i = 0; i < instr->num_definitions; i++) {
            /* erase from live */
            if (instr->getDefinition(i).isTemp())
               live.erase(instr->getDefinition(i).getTemp());
         }
      }
   }

   for (std::unique_ptr<Block>& block : program->blocks) {
      std::set<Temp>& live = live_out_per_block[block->index];
      std::map<unsigned, Instruction*>& kills = kills_per_block[block->index];
      /* initialize register file */
      assert(block->index != 0 || live.empty());
      std::array<uint32_t, 512> register_file = {0};

      for (Temp t : live) {
         assert(assignments.find(t.id()) != assignments.end());
         for (unsigned i = 0; i < t.size(); i++)
            register_file[assignments[t.id()].first.reg + i] = t.id();
      }

      std::vector<std::unique_ptr<Instruction>> instructions;
      std::unique_ptr<Instruction> pc;
      std::vector<std::unique_ptr<Instruction>>::iterator it;
      for (it = block->instructions.begin(); it != block->instructions.end(); ++it) {
         std::unique_ptr<Instruction>& instr = *it;
         std::vector<std::pair<Operand, Definition>> parallelcopy;
         /* this is a slight adjustment from the paper as we already have phi nodes:
          * We consider them incomplete phis. */
         if (instr->opcode == aco_opcode::p_phi || instr->opcode == aco_opcode::p_linear_phi) {
            Definition def = instr->getDefinition(0);
            renames[block->index][def.tempId()] = def.getTemp();
         } else {
            /* handle operands */
            for (unsigned i = 0; i < instr->num_operands; ++i) {
               auto& operand = instr->getOperand(i);
               if (!operand.isTemp())
                  continue;

               /* unset register file bits if it's the last use */
               std::map<unsigned, Instruction*>::iterator it = kills.find(operand.tempId());
               if (it != kills.end() && it->second == instr.get()) {
                  operand.setKill(true);
               }

               /* rename operands */
               operand.setTemp(read_variable(operand.getTemp(), block.get()));

               /* check if the operand is fixed */
               if (operand.isFixed()) {
                  if (operand.physReg() == assignments[operand.tempId()].first) {
                     /* we are fine: the operand is already assigned the correct reg */

                  } else {
                     /* check if target reg is blocked, and move away the blocking var */
                     if (register_file[operand.physReg().reg]) {
                        uint32_t blocking_id = register_file[operand.physReg().reg];
                        Operand pc_op = Operand(Temp{blocking_id, assignments[blocking_id].second});
                        pc_op.setFixed(operand.physReg());
                        Definition pc_def = Definition(Temp{program->allocateId(), pc_op.regClass()});
                        /* find free reg */
                        PhysReg reg = get_reg(register_file, pc_op.regClass(), parallelcopy, instr);
                        pc_def.setFixed(reg);
                        assignments[pc_def.tempId()] = {reg, pc_def.regClass()};
                        for (unsigned i = 0; i < operand.size(); i++) {
                           register_file[pc_op.physReg().reg + i] = 0;
                           register_file[pc_def.physReg().reg + i] = pc_def.tempId();
                        }
                        parallelcopy.emplace_back(pc_op, pc_def);
                     }
                     /* move operand to fixed reg and create parallelcopy pair */
                     Operand pc_op = operand;
                     Temp tmp = Temp{program->allocateId(), operand.regClass()};
                     Definition pc_def = Definition(tmp);
                     pc_def.setFixed(operand.physReg());
                     pc_op.setFixed(assignments[operand.tempId()].first);
                     operand = Operand(tmp);
                     assignments[tmp.id()] = {pc_def.physReg(), pc_def.regClass()};
                     operand.setFixed(pc_def.physReg());
                     for (unsigned i = 0; i < operand.size(); i++) {
                        register_file[pc_op.physReg().reg + i] = 0;
                        register_file[pc_def.physReg().reg + i] = tmp.id();
                     }
                     parallelcopy.emplace_back(pc_op, pc_def);
                  }
               } else {
                  assert(assignments.find(operand.tempId()) != assignments.end());
                  operand.setFixed(assignments[operand.tempId()].first);
               }
               std::map<unsigned, phi_info>::iterator phi = phi_map.find(operand.getTemp().id());
               if (phi != phi_map.end())
                  phi->second.uses.emplace(instr.get());

            }
            /* remove dead vars from register file */
            for (unsigned i = 0; i < instr->num_operands; i++)
               if (instr->getOperand(i).isFixed() && instr->getOperand(i).isKill())
                  for (unsigned j = 0; j < instr->getOperand(i).size(); j++)
                     register_file[instr->getOperand(i).physReg().reg + j] = 0;
         }

         /* handle definitions */
         for (unsigned i = 0; i < instr->num_definitions; ++i) {
            auto& definition = instr->getDefinition(i);
            if (!definition.isTemp())
               continue;
            if (definition.isFixed()) {
               /* check if target dst is blocked */
               if (register_file[definition.physReg().reg] != 0) {
                  /* create parallelcopy pair to move blocking var */
                  Temp tmp = {register_file[definition.physReg().reg], assignments[register_file[definition.physReg().reg]].second};
                  Operand pc_op = Operand(tmp);
                  pc_op.setFixed(assignments[register_file[definition.physReg().reg]].first);
                  tmp = Temp{program->allocateId(), pc_op.regClass()};
                  Definition pc_def = Definition(tmp);
                  PhysReg reg = get_reg(register_file, pc_op.regClass(), parallelcopy, instr);
                  pc_def.setFixed(reg);
                  assignments[pc_def.tempId()] = {reg, pc_def.regClass()};
                  for (unsigned i = 0; i < pc_op.size(); i++) {
                     register_file[pc_op.physReg().reg + i] = 0xFFFF;
                     register_file[pc_def.physReg().reg + i] = pc_def.tempId();
                  }
                  parallelcopy.emplace_back(pc_op, pc_def);
               }
            } else {
               /* find free reg */
               if (instr->opcode == aco_opcode::v_interp_p2_f32 ||
                   instr->opcode == aco_opcode::v_mac_f32)
                  definition.setFixed(instr->getOperand(2).physReg());
               else if (instr->opcode == aco_opcode::p_split_vector &&
                        register_file[instr->getOperand(0).physReg().reg + i] == 0)
                  definition.setFixed(PhysReg{instr->getOperand(0).physReg().reg + i});
               else if (definition.hasHint() && register_file[definition.physReg().reg] == 0)
                  definition.setFixed(definition.physReg());
               else if (affinities.find(definition.tempId()) != affinities.end() &&
                        assignments.find(affinities[definition.tempId()]) != assignments.end()) {
                  PhysReg reg = assignments[affinities[definition.tempId()]].first;
                  for (unsigned i = 0; i < definition.size(); i++) {
                     if (register_file[reg.reg + i] != 0) {
                        definition.setFixed(get_reg(register_file, definition.regClass(), parallelcopy, instr));
                        break;
                     }
                  }
                  if (!definition.isFixed())
                     definition.setFixed(reg);
               } else
                  definition.setFixed(get_reg(register_file, definition.regClass(), parallelcopy, instr));
            }

            assignments[definition.tempId()] = {definition.physReg(), definition.regClass()};
            /* set live if it has a kill point */
            if (kills.find(definition.tempId()) != kills.end()) {
               for (unsigned i = 0; i < definition.size(); i++)
                  register_file[definition.physReg().reg + i] = definition.tempId();
               live.emplace(definition.getTemp());
            }
            /* add to renames table */
            renames[block->index][definition.tempId()] = definition.getTemp();
         }

         /* emit parallelcopy */
         if (!parallelcopy.empty()) {
            pc.reset(create_instruction<Instruction>(aco_opcode::p_parallelcopy, Format::PSEUDO, parallelcopy.size(), parallelcopy.size()));
            for (unsigned i = 0; i < parallelcopy.size(); i++) {
               pc->getOperand(i) = parallelcopy[i].first;
               pc->getDefinition(i) = parallelcopy[i].second;

               /* it might happen that the operand is already renamed. we have to restore the original name. */
               std::map<unsigned, Temp>::iterator it = orig_names.find(pc->getOperand(i).tempId());
               if (it != orig_names.end())
                  pc->getOperand(i).setTemp(it->second);
               unsigned orig_id = pc->getOperand(i).tempId();
               orig_names[pc->getDefinition(i).tempId()] = pc->getOperand(i).getTemp();

               pc->getOperand(i).setTemp(read_variable(pc->getOperand(i).getTemp(), block.get()));
               renames[block->index][orig_id] = pc->getDefinition(i).getTemp();
               std::map<unsigned, phi_info>::iterator phi = phi_map.find(pc->getOperand(i).tempId());
               if (phi != phi_map.end())
                  phi->second.uses.emplace(instr.get());
            }
            instructions.emplace_back(std::move(pc));
         }
         if (instr->opcode == aco_opcode::v_add_co_u32 && !(instr->getDefinition(1).physReg() == vcc)) {
            /* change the instruction to VOP3 to enable an arbitrary register pair as dst */
            std::unique_ptr<Instruction> tmp = std::move(instr);
            Format format = (Format) ((int) tmp->format | (int) Format::VOP3A);
            instr.reset(create_instruction<VOP3A_instruction>(tmp->opcode, format, tmp->num_operands, tmp->num_definitions));
            for (unsigned i = 0; i < instr->num_operands; i++)
               instr->getOperand(i) = tmp->getOperand(i);
            for (unsigned i = 0; i < instr->num_definitions; i++)
               instr->getDefinition(i) = tmp->getDefinition(i);
         }
         instructions.emplace_back(std::move(*it));
      } /* end for Instr */
      block->instructions = std::move(instructions);

      filled[block->index] = true;
      for (Block* succ : block->linear_successors) {
         /* seal block if all predecessors are filled */
         bool all_filled = true;
         for (Block* pred : succ->linear_predecessors) {
            if (!filled[pred->index]) {
               all_filled = false;
               break;
            }
         }
         if (all_filled) {
            /* finish incomplete phis and check if they became trivial */
            for (std::unique_ptr<Instruction>& phi : incomplete_phis[succ->index]) {
               std::vector<Block*> preds = phi->getDefinition(0).getTemp().type() == vgpr ? succ->logical_predecessors : succ->linear_predecessors;
               for (unsigned i = 0; i < phi->num_operands; i++) {
                  phi->getOperand(i).setTemp(read_variable(phi->getOperand(i).getTemp(), preds[i]));
                  phi->getOperand(i).setFixed(assignments[phi->getOperand(i).tempId()].first);
               }
               try_remove_trivial_phi(phi_map.find(phi->getDefinition(0).tempId()));
            }
            /* complete the original phi nodes, but no need to check triviality */
            for (std::unique_ptr<Instruction>& instr : succ->instructions) {
               if (instr->opcode != aco_opcode::p_phi && instr->opcode != aco_opcode::p_linear_phi)
                  break;
               std::vector<Block*> preds = instr->opcode == aco_opcode::p_phi ? succ->logical_predecessors : succ->linear_predecessors;

               for (unsigned i = 0; i < instr->num_operands; i++) {
                  auto& operand = instr->getOperand(i);
                  if (!operand.isTemp())
                     continue;
                  operand.setTemp(read_variable(operand.getTemp(), preds[i]));
                  operand.setFixed(assignments[operand.tempId()].first);
                  std::map<unsigned, phi_info>::iterator phi = phi_map.find(operand.getTemp().id());
                  if (phi != phi_map.end())
                     phi->second.uses.emplace(instr.get());
               }
            }
            /* merge incomplete phis */
            phis[succ->index].insert(phis[succ->index].end(),
                                     std::make_move_iterator(incomplete_phis[succ->index].begin()),
                                     std::make_move_iterator(incomplete_phis[succ->index].end()));
            sealed[succ->index] = true;
         }
      }
   } /* end for BB */

   /* merge new phis with normal instructions */
   for (std::unique_ptr<Block>& block : program->blocks) {
      std::vector<std::unique_ptr<Instruction>> tmp;
      std::vector<std::unique_ptr<Instruction>>::iterator it = phis[block->index].begin();
      while (it != phis[block->index].end()) {
         if ((*it)->num_definitions != 0)
            tmp.emplace_back(std::move(*it));
         ++it;
      }
      block->instructions.insert(block->instructions.begin(),
                                 std::make_move_iterator(tmp.begin()),
                                 std::make_move_iterator(tmp.end()));
   }
}

}