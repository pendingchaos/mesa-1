#include <vector>

#include "aco_ir.h"

namespace aco {

struct asm_context {
   // TODO: keep track of branch instructions referring blocks
   // and, when emitting the block, correct the offset in instr
};

void emit_instruction(asm_context ctx, std::vector<uint32_t>& out, Instruction* instr)
{
   switch (instr->format)
   {
   // ideally, we don't want to check the number of operands or definitions
   // but only bitwise_or them, if a field can be def or op
   case Format::VOP3A: {
      uint32_t encoding = (0b110100 << 26);
      encoding |= opcode_infos[(int)instr->opcode].opcode << 16;
      // TODO: clmp, abs, op_sel
      encoding |= (0xFF & instr->getDefinition(0).physReg().reg);
      out.push_back(encoding);
      // TODO: omod, neg
      encoding = 0;
      for (unsigned i = 0; i < instr->operandCount(); i++)
         encoding |= instr->getOperand(i).physReg().reg << (i * 9);
      out.push_back(encoding);
      break;
   }
   case Format::SOP2: {
      uint32_t encoding = (0b10 << 30);
      encoding |= opcode_infos[(int)instr->opcode].opcode << 23;
      encoding |= instr->definitionCount() ? instr->getDefinition(0).physReg().reg << 16 : 0;
      encoding |= instr->operandCount() == 2 ? instr->getOperand(1).physReg().reg << 8 : 0;
      encoding |= instr->operandCount() ? instr->getOperand(0).physReg().reg : 0;
      out.push_back(encoding);
      break;
   }
   case Format::SOPK: {
      uint32_t encoding = (0b1011 << 28);
      encoding |= opcode_infos[(int)instr->opcode].opcode << 23;
      encoding |=
         instr->definitionCount() && instr->getDefinition(0).regClass() != RegClass::b ?
         instr->getDefinition(0).physReg().reg << 16 :
         instr->operandCount() && instr->getOperand(0).regClass() != RegClass::b ?
         instr->getOperand(0).physReg().reg << 16 : 0;
      encoding |= static_cast<SOPK_instruction*>(instr)->imm;
      out.push_back(encoding);
      break;
   }
   case Format::SOP1: {
      uint32_t encoding = (0b101111101 << 23);
      encoding |= instr->definitionCount() ? instr->getDefinition(0).physReg().reg << 16 : 0;
      encoding |= opcode_infos[(int)instr->opcode].opcode << 8;
      encoding |= instr->operandCount() ? instr->getOperand(0).physReg().reg : 0;
      out.push_back(encoding);
      break;
   }
   case Format::SOPP: {
      uint32_t encoding = (0b101111111 << 23);
      encoding |= opcode_infos[(int)instr->opcode].opcode << 16;
      encoding |= static_cast<SOPP_instruction*>(instr)->imm;
      out.push_back(encoding);
      break;
   }
   case Format::VOP2: {
      uint32_t encoding = 0;
      encoding |= opcode_infos[(int)instr->opcode].opcode << 25;
      encoding |= (0xFF & instr->getDefinition(0).physReg().reg) << 17;
      encoding |= instr->getOperand(1).physReg().reg << 9;
      encoding |= instr->getOperand(0).physReg().reg;
      out.push_back(encoding);
      break;
   }
   case Format::VOP1: {
      uint32_t encoding = (0b0111111 << 25);
      encoding |= (0xFF & instr->getDefinition(0).physReg().reg) << 17;
      encoding |= opcode_infos[(int)instr->opcode].opcode << 9;
      encoding |= instr->getOperand(0).physReg().reg;
      out.push_back(encoding);
      break;
   }
   case Format::VOPC: {
      uint32_t encoding = (0b0111110 << 25);
      encoding |= opcode_infos[(int)instr->opcode].opcode << 17;
      encoding |= (0xFF & instr->getOperand(1).physReg().reg) << 9;
      encoding |= instr->getOperand(0).physReg().reg;
      out.push_back(encoding);
      break;
   }
   case Format::VINTRP: {
      Interp_instruction* interp = static_cast<Interp_instruction*>(instr);
      uint32_t encoding = (0b110101 << 26);
      encoding |= (0xFF & instr->getDefinition(0).physReg().reg) << 18;
      encoding |= opcode_infos[(int)instr->opcode].opcode << 16;
      encoding |= interp->attribute << 10;
      encoding |= interp->component << 8;
      encoding |= (0xFF & instr->getOperand(0).physReg().reg);
      out.push_back(encoding);
      break;
   }
   case Format::EXP: {
      Export_instruction* exp = static_cast<Export_instruction*>(instr);
      uint32_t encoding = (0b110001 << 26);
      encoding |= exp->valid_mask ? 0b1 << 12 : 0;
      encoding |= exp->done ? 0b1 << 11 : 0;
      encoding |= exp->compressed ? 0b1 << 10 : 0;
      encoding |= exp->dest << 4;
      encoding |= exp->enabled_mask;
      out.push_back(encoding);
      encoding = 0xFF & exp->getOperand(0).physReg().reg;
      encoding |= (0xFF & exp->getOperand(1).physReg().reg) << 8;
      encoding |= (0xFF & exp->getOperand(2).physReg().reg) << 16;
      encoding |= (0xFF & exp->getOperand(3).physReg().reg) << 24;
      out.push_back(encoding);
      break;
   }
   case Format::PSEUDO:
      break;
   default:
      unreachable("unimplemented instruction format");
   }

   /* append literal dword */
   if (instr->operandCount() && instr->getOperand(0).physReg().reg == 255)
   {
      uint32_t literal = instr->getOperand(0).constantValue();
      out.push_back(literal);
   }
}

void emit_block(asm_context ctx, std::vector<uint32_t>& out, Block* block)
{
   // TODO: emit offsets on previous branches to this block
   //std::iostream::pos_type current = out.tellp();
   
   for (auto const& instr : block->instructions)
      emit_instruction(ctx, out, instr.get());
}

void emit_elf_header(asm_context ctx, std::vector<uint32_t>& out, Program* program)
{
   // TODO
}

std::vector<uint32_t> emit_program(Program* program)
{
   // TODO: initialize context
   asm_context ctx;
   std::vector<uint32_t> out;
   emit_elf_header(ctx, out, program);
   for (auto const& block : program->blocks)
      emit_block(ctx, out, block.get());
   // footer?
   return out;
}

}