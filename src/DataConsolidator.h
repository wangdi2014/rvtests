#ifndef _DATACONSOLIDATOR_H_
#define _DATACONSOLIDATOR_H_

#include "base/Logger.h"
#include "base/ParRegion.h"
#include "libsrc/MathMatrix.h"
#include "libsrc/Random.h"

#include "KinshipHolder.h"
#include "Result.h"

extern Logger* logger;

class WarningOnce {
 public:
  WarningOnce(const std::string& msg) : warningGiven(false), msg(msg){};
  void warningIf(bool cond) {
    if (cond && !warningGiven) {
      warningGiven = true;
      fprintf(stderr, "%s", msg.c_str());
    }
  }

 private:
  bool warningGiven;
  std::string msg;
};

class EigenMatrix;

/**
 * Impute missing genotype (<0) according to population frequency (p^2, 2pq,
 * q^2)
 */
inline void imputeGenotypeByFrequency(Matrix* genotype, Random* r) {
  Matrix& m = *genotype;
  for (int i = 0; i < m.cols; i++) {
    int ac = 0;
    int an = 0;
    for (int j = 0; j < m.rows; j++) {
      if (m[j][i] >= 0) {
        ac += m[j][i];
        an += 2;
      }
    }
    double p = an == 0 ? 0 : 1.0 * ac / an;
    double pRef = p * p;
    double pHet = pRef + 2.0 * p * (1.0 - p);
    for (int j = 0; j < m.rows; j++) {
      if (m[j][i] < 0) {
        double v = r->Next();
        if (v < pRef) {
          m[j][i] = 0;
        } else if (v < pHet) {
          m[j][i] = 1;
        } else {
          m[j][i] = 2;
        }
      }
    }
  }
};

/**
 * Impute missing genotype (<0) according to its mean genotype
 * @param genotype (people by marker matrix)
 */
inline void imputeGenotypeToMean(Matrix* genotype) {
  Matrix& m = *genotype;
  for (int i = 0; i < m.cols; i++) {
    int ac = 0;
    int an = 0;
    for (int j = 0; j < m.rows; j++) {
      if (m[j][i] >= 0) {
        ac += m[j][i];
        an += 2;
      }
    }
    double p;
    if (an == 0) {
      p = 0.0;
    } else {
      p = 1.0 * ac / an;
    }
    double g = 2.0 * p;
    for (int j = 0; j < m.rows; j++) {
      if (m[j][i] < 0) {
        m[j][i] = g;
      }
    }
    // fprintf(stderr, "impute to mean = %g, ac = %d, an = %d\n", g, ac, an);
  }
};

/**
 * @return true if any of the markers (@param col) of @param genotype (people by
 * marker) is missing
 */
inline bool hasMissingMarker(Matrix& genotype, int col) {
  if (col >= genotype.cols || col < 0) {
    logger->error("Invalid check of missing marker.");
    return false;
  }

  for (int r = 0; r < genotype.rows; ++r) {
    if (genotype[r][col] < 0) return true;
  }
  return false;
};

/**
 * Remove columns of markers in @param genotype (people by marker) where there
 * are missing genotypes
 */
void removeMissingMarker(Matrix* genotype);

/**
 * @return true if markers on @param col of @param genotype (people by marker)
 * is monomorphic (genotypes are all the same)
 */
bool isMonomorphicMarker(Matrix& genotype, int col);

/**
 * remove monomorphic columns of @param genotype
 */
void removeMonomorphicMarker(Matrix* genotype);

/**
 * Convert genotype back to reference allele count
 * e.g. genotype 2 means homAlt/homAlt, so it has reference allele count 0
 */
void convertToMinorAlleleCount(Matrix& in, Matrix* g);

/**
 * This class counts either hard genotype calls or dosage
 */
class GenotypeCounter {
 public:
  GenotypeCounter() { reset(); }
  void reset() {
    nHomRef = nHet = nHomAlt = nMissing = nSample = 0;
    sumAC = 0.;
  }
  void add(const double g) {
    // to accomodate dosage (g), we will use these thresholds for dosages
    // 0 <= g < 1  => (1 - g) homRef and (g) het
    // 1 <= g < 2  => (2 - g) het and (g - 1) homAlt
    if (g < 0) {
      ++(nMissing);
    }
    if (g < 2.0 / 3) {
      ++(nHomRef);
      sumAC += g;
    } else if (g < 4.0 / 3) {
      ++(nHet);
      sumAC += g;
    } else if (g <= 2.0) {
      ++(nHomAlt);
      sumAC += g;
    } else {
      ++(nMissing);
    }
    ++nSample;
  }
  int getNumHomRef() const { return this->nHomRef; }
  int getNumHet() const { return this->nHet; }
  int getNumHomAlt() const { return this->nHomAlt; }
  int getNumMissing() const { return this->nMissing; }
  int getNumSample() const { return this->nSample; }
  double getCallRate() const {
    double callRate = 0.0;
    if (nSample) {
      callRate = 1.0 - 1.0 * nMissing / nSample;
    }
    return callRate;
  }
  double getAF() const {
    double af = -1.0;
    if (nSample) {
      af = 0.5 * sumAC / nSample; 
    }
    return af;
  }
  // total alternative allele counts
  double getAC() const { return sumAC; }
  double getHWE() const;

 private:
  int nHomRef;
  int nHet;
  int nHomAlt;
  int nMissing;
  int nSample;
  double sumAC;
};
/**
 * This class is in charge of cleanning data before fitting in model
 * The cleaning step includes:
 *  remove monomorphic sites
 *  handle missing data genotype (impute to mean, impute by HWE, or
 *   filter out mssing genotypes and its corresponding phenotypes, covariates)
 *  (future todo) include weights (GERP, Sift)
 *  (future todo) re-weight genotype (dominate model, recessive model)
 */
class DataConsolidator {
 public:
  const static int UNINITIALIZED = 0;
  const static int IMPUTE_MEAN = 1;
  const static int IMPUTE_HWE = 2;
  const static int DROP = 3;
  const static int KINSHIP_AUTO = 0;
  const static int KINSHIP_X = 1;
  typedef enum { ANY_SEX = -1, MALE = 1, FEMALE = 2 } PLINK_SEX;
  typedef enum { ANY_PHENO = -1, CTRL = 1, CASE = 2 } PLINK_PHENOTYPE;

 public:
  DataConsolidator();
  virtual ~DataConsolidator();
  void setStrategy(const int s) { this->strategy = s; };
  /**
   * @param pheno, @param cov @param genotype are all ordered and sorted by the
   * same people
   * @param phenoOut, @param covOut and @param genotype are outputted
   * NOTE: @param covOut may be filled as column vector of 1 if @param cov is
   * empty
   */
  void consolidate(Matrix& pheno, Matrix& cov, Matrix& geno) {
    this->originalGenotype = geno;
    this->genotype = geno;

    copyColName(pheno, &this->phenotype);
    copyColName(cov, &this->covariate);
    copyColName(geno, &this->genotype);
    copyColName(geno, &this->originalGenotype);

    if (this->strategy == IMPUTE_MEAN) {
      // impute missing genotypes
      imputeGenotypeToMean(&this->genotype);
      if (this->phenotype != pheno) {
        this->phenotype = pheno;
        this->phenotypeUpdated = true;
      } else {
        this->phenotypeUpdated = false;
      }
      if (this->covariate != cov) {
        this->covariate = cov;
        this->covariateUpdated = true;
      } else {
        this->covariateUpdated = false;
      }
    } else if (this->strategy == IMPUTE_HWE) {
      // impute missing genotypes
      imputeGenotypeByFrequency(&genotype, &this->random);
      /* *phenoOut = pheno; */
      /* *covOut = cov; */
      this->phenotype = pheno;
      this->covariate = cov;
    } else if (this->strategy == DROP) {
      // XX: should also consider how kinship matrix changes.

      // we process genotype matrix (people by marker)
      // if for the same people, any marker is empty, we will remove this people
      int idxToCopy = 0;
      for (int i = 0; i < (genotype).rows; ++i) {
        if (hasNoMissingGenotype(genotype, i)) {
          copyRow(genotype, i, &genotype, idxToCopy);
          copyRow(cov, i, &covariate, idxToCopy);
          copyRow(pheno, i, &phenotype, idxToCopy);
          rowLabel[idxToCopy] = originalRowLabel[i];
          idxToCopy++;
        }
      }
      this->phenotypeUpdated = this->covariateUpdated = true;
      genotype.Dimension(idxToCopy, genotype.cols);
      covariate.Dimension(idxToCopy, cov.cols);
      phenotype.Dimension(idxToCopy, pheno.cols);
    } else {
      logger->error(
          "Uninitialized consolidation methods to handle missing data!");
    }
  }
  bool hasNoMissingGenotype(Matrix& g, int r) {
    const int n = g.cols;
    for (int i = 0; i < n; ++i) {
      if (g[r][i] < 0) return false;
    }
    return true;
  }
  void copyRow(Matrix& src, const int srcRow, Matrix* dest, const int destRow) {
    Matrix& m = *dest;
    if (m.cols < src.cols) {
      m.Dimension(m.rows, src.cols);
    }
    if (m.rows <= destRow) {
      m.Dimension(destRow + 1, m.cols);
    }
    const int n = m.cols;
    for (int i = 0; i < n; ++i) {
      m[destRow][i] = src[srcRow][i];
    }
  }
  void copyColName(Matrix& src, Matrix* dest) {
    dest->Dimension(dest->rows, src.cols);
    for (int i = 0; i < src.cols; ++i) {
      dest->SetColumnLabel(i, src.GetColumnLabel(i));
    }
  }
  void setPhenotypeName(const std::vector<std::string>& name) {
    this->originalRowLabel = name;
    this->rowLabel = name;
  }
  const std::vector<std::string>& getRowLabel() const { return this->rowLabel; }
  Matrix& getGenotype() { return this->genotype; }
  Matrix& getFlippedToMinorPolymorphicGenotype() {
    convertToMinorAlleleCount(this->genotype, &this->flippedToMinorGenotype);
    removeMonomorphicMarker(&flippedToMinorGenotype);
    return this->flippedToMinorGenotype;
  }
  Matrix& getPhenotype() { return this->phenotype; }
  Matrix& getCovariate() { return this->covariate; }
  Vector& getWeight() { return this->weight; }
  Result& getResult() { return this->result; }

  /**
   * Count @param homRef, @param het, @param homAlt and @param missing
   * from the genotype column specified by @param columnIndex
   * @param sex : only process specified sex (1, male; 2, female; <0, any)
   * @param phenotype: only process specified phenotype (1, control; 2, case; <0
   * any)
   * @return 0 if succeed
   */
  int countRawGenotype(int columnIndex, const PLINK_SEX sex,
                       const PLINK_PHENOTYPE phenotype,
                       GenotypeCounter* counter) const {
    if (columnIndex < 0 || columnIndex >= originalGenotype.cols) {
      return -1;
    }
    if (sex > 0 && sex != MALE && sex != FEMALE) return -2;
    if (sex > 0 && (int)this->sex->size() != originalGenotype.rows) return -3;
    if (phenotype > 0 && phenotype != CTRL && phenotype != CASE) return -2;

    for (int i = 0; i < originalGenotype.rows; ++i) {
      if (sex > 0 && (*this->sex)[i] != sex) {
        continue;
      }
      // + 1: PLINK use 1 and 2 as ctrl and case, but
      // internally, we use 0 and 1.
      if (phenotype > 0 && (int)(this->phenotype[i][0] + 1) != phenotype) {
        continue;
      }

      const double& g = originalGenotype[i][columnIndex];
      counter->add(g);
    }

    return 0;  // success
  }

  int countRawGenotypeFromCase(int columnIndex,
                               GenotypeCounter* counter) const {
    return countRawGenotype(columnIndex,
                            ANY_SEX,  // PLINK male
                            CASE, counter);
  }

  int countRawGenotypeFromControl(int columnIndex,
                                  GenotypeCounter* counter) const {
    return countRawGenotype(columnIndex, ANY_SEX,
                            CTRL,  // any phenotype
                            counter);
  }

  int countRawGenotype(int columnIndex, GenotypeCounter* counter) const {
    return countRawGenotype(columnIndex, ANY_SEX,
                            ANY_PHENO,  // any phenotype
                            counter);
  }
  int countRawGenotypeFromFemale(int columnIndex,
                                 GenotypeCounter* counter) const {
    return countRawGenotype(columnIndex, FEMALE,
                            ANY_PHENO,  // any phenotype
                            counter);
  }
  int countRawGenotypeFromFemaleCase(int columnIndex,
                                     GenotypeCounter* counter) const {
    return countRawGenotype(columnIndex, FEMALE, CASE, counter);
  }
  int countRawGenotypeFromFemaleControl(int columnIndex,
                                        GenotypeCounter* counter) const {
    return countRawGenotype(columnIndex, FEMALE, CTRL, counter);
  }

  bool isPhenotypeUpdated() const { return this->phenotypeUpdated; }
  bool isCovariateUpdated() const { return this->covariateUpdated; }

  void setParRegion(ParRegion* p) { this->parRegion = p; }

  // Sex (1=male; 2=female; other=unknown)
  void setSex(const std::vector<int>* sex) { this->sex = sex; };
  /**
   * Check if genotype matrix column @param columnIndex is a chromosome X.
   */
  bool isHemiRegion(int columnIndex) {
    assert(this->parRegion);
    std::string chromPos = this->genotype.GetColumnLabel(0);
    size_t posColon = chromPos.find(":");
    if (posColon == std::string::npos) return false;
    std::string chrom = chromPos.substr(0, posColon);
    int pos = atoi(chromPos.substr(posColon + 1));
    return this->parRegion->isHemiRegion(chrom, pos);
  }

  void codeGenotypeForDominantModel(Matrix* geno) {
    int n = genotype.cols;
    static WarningOnce warning("Encoding only use the first variant!\n");
    warning.warningIf(n != 1);

    int m = genotype.rows;
    if (n != 1) {
      fprintf(stderr, "n = %d, m = %d \n", n, m);
    }

    geno->Dimension(m, 1);
    double s = 0;  // sum of genotypes
    int numGeno = 0;
    if (this->strategy == IMPUTE_MEAN || this->strategy == IMPUTE_HWE) {
      for (int i = 0; i < m; ++i) {
        if (this->originalGenotype[i][0] < 0) continue;

        if (this->originalGenotype[i][0] > 0.5) {
          (*geno)[i][0] = 1.0;
          s += 1.;
          numGeno++;
        } else {
          (*geno)[i][0] = 0.0;
          numGeno++;
        }
      }
      double avg = 0.0;
      if (numGeno > 0) {
        avg = s / numGeno;
      }
      for (int i = 0; i < m; ++i) {
        if (this->originalGenotype[i][0] < 0) (*geno)[i][0] = avg;
      }
    } else if (this->strategy == DROP) {
      for (int i = 0; i < m; ++i) {
        if (this->genotype[i][0] > 0.5) {
          (*geno)[0][i] = 1.;
        } else {
          (*geno)[0][i] = 0.;
        }
      }
    }
  }
  void codeGenotypeForRecessiveModel(Matrix* geno) {
    int n = genotype.cols;
    static WarningOnce warning("Encoding only use the first variant!\n");
    warning.warningIf(n != 1);

    int m = genotype.rows;
    geno->Dimension(m, 1);
    double s = 0;  // sum of genotypes
    int numGeno = 0;
    if (this->strategy == IMPUTE_MEAN || this->strategy == IMPUTE_HWE) {
      for (int i = 0; i < m; ++i) {
        if (this->originalGenotype[i][0] < 0) continue;

        if (this->originalGenotype[i][0] > 1.5) {
          (*geno)[i][0] = 1.0;
          s += 1.;
          numGeno++;
        } else {
          (*geno)[i][0] = 0.0;
          numGeno++;
        }
      }
      double avg = 0.0;
      if (numGeno > 0) {
        avg = s / numGeno;
      }
      for (int i = 0; i < m; ++i) {
        if (this->originalGenotype[i][0] < 0) (*geno)[i][0] = avg;
      }
    } else if (this->strategy == DROP) {
      for (int i = 0; i < m; ++i) {
        if (this->genotype[i][0] > 1.5) {
          (*geno)[0][i] = 1.;
        } else {
          (*geno)[0][i] = 0.;
        }
      }
    }
  }

 public:
  // codes to check before regression
  int preRegressionCheck(Matrix& pheno, Matrix& cov);
  int checkColinearity(Matrix& cov);
  int checkPredictor(Matrix& pheno, Matrix& cov);  

 public:
  // codes related to kinship
  int setKinshipSample(const std::vector<std::string>& samples);
  int setKinshipFile(int kinshipType, const std::string& fileName);
  int setKinshipEigenFile(int kinshipType, const std::string& fileName);
  int loadKinship(int kinshipType);

  const EigenMatrix* getKinshipForAuto() const {
    return this->kinship[KINSHIP_AUTO].getK();
  }
  const EigenMatrix* getKinshipUForAuto() const {
    return this->kinship[KINSHIP_AUTO].getU();
  }
  const EigenMatrix* getKinshipSForAuto() const {
    return this->kinship[KINSHIP_AUTO].getS();
  }
  bool hasKinshipForAuto() const {
    return this->kinship[KINSHIP_AUTO].isLoaded();
  }

  const EigenMatrix* getKinshipForX() const {
    return this->kinship[KINSHIP_X].getK();
  }
  const EigenMatrix* getKinshipUForX() const {
    return this->kinship[KINSHIP_X].getU();
  }
  const EigenMatrix* getKinshipSForX() const {
    return this->kinship[KINSHIP_X].getS();
  }
  bool hasKinshipForX() const { return this->kinship[KINSHIP_X].isLoaded(); }

  bool hasKinship() const {
    return this->hasKinshipForAuto() || this->hasKinshipForX();
  }

 private:
  // don't copy
  DataConsolidator(const DataConsolidator&);
  DataConsolidator& operator=(const DataConsolidator&);

 private:
  int strategy;
  Random random;
  Matrix genotype;
  Matrix flippedToMinorGenotype;
  Matrix phenotype;
  Matrix covariate;
  Vector weight;
  Result result;
  Matrix originalGenotype;
  bool phenotypeUpdated;
  bool covariateUpdated;
  std::vector<std::string> originalRowLabel;
  std::vector<std::string> rowLabel;

  KinshipHolder kinship[2];  // 2: include both AUTO and X kinships

  // sex chromosome adjustment
  const std::vector<int>* sex;

  ParRegion* parRegion;
};  // end DataConsolidator

#endif /* _DATACONSOLIDATOR_H_ */
